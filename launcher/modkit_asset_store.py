"""Read-only offline asset browsing through the existing native format tools."""
from collections import OrderedDict
import json
from pathlib import Path
import subprocess
import tempfile
import threading

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal, Slot
if __package__:
    from .modkit_geometry import mesh_buffers
else:
    from modkit_geometry import mesh_buffers


class AssetSignals(QObject):
    completed = Signal(object, object, str)
    progress = Signal(str)


class AssetJob(QRunnable):
    def __init__(self, function):
        super().__init__(); self.setAutoDelete(False)
        self.function = function; self.signals = AssetSignals()

    def run(self):
        value, error = None, ''
        try: value = self.function(self.signals.progress.emit)
        except Exception as failure: error = str(failure)
        self.signals.completed.emit(self, value, error)


class AssetStore(QObject):
    changed = Signal()
    progress = Signal(str)
    meshReady = Signal(str, object)
    meshFailed = Signal(str, str)

    def __init__(self, tool, game, overlays=(), parent=None, environment=None):
        super().__init__(parent)
        self.tool, self.game = Path(tool), Path(game)
        self.overlays = [Path(path) for path in overlays if Path(path).is_dir()]
        self.environment=environment
        self.temporary = tempfile.TemporaryDirectory(prefix='simpsons-modkit-assets-')
        self.folder = Path(self.temporary.name)
        self.data = None; self.by_id = {}; self.by_guid = {}
        self.cache = OrderedDict(); self.cache_bytes = 0
        self.pool = QThreadPool(self); self.pool.setMaxThreadCount(1)
        self.job = None; self.wanted = None; self.process = None
        self.lock = threading.Lock(); self.closed = False; self.serial = 0

    def command(self, arguments, progress=None):
        if self.closed: raise ValueError('Asset browser closed')
        if not self.tool.is_file():
            raise ValueError('Install the Modkit asset tools to browse and preview files without starting the game')
        with self.lock:
            if self.closed: raise ValueError('Asset browser closed')
            process = subprocess.Popen([str(self.tool), *map(str, arguments)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace',
                env=self.environment,
                **({'creationflags':subprocess.CREATE_NO_WINDOW} if hasattr(subprocess,'CREATE_NO_WINDOW') else {}))
            self.process = process
        lines = []
        try:
            for line in process.stdout:
                if len(lines) == 12: lines.pop(0)
                lines.append(line.strip())
                if progress: progress(line.strip())
            code = process.wait(timeout=10)
            if code: raise ValueError('\n'.join(lines) or 'The native asset reader failed')
            if self.closed: raise ValueError('Asset browser closed')
        finally:
            process.stdout.close()
            with self.lock:
                if self.process is process: self.process = None

    def submit(self, kind, function):
        if self.closed or self.job: return
        job = AssetJob(function); job.kind = kind; self.job = job
        job.signals.completed.connect(self.completed)
        job.signals.progress.connect(self.report_progress)
        self.pool.start(job)

    def ensure(self):
        if self.data or self.job or self.closed: return
        def index(progress):
            output=self.folder/'library.json'
            output.unlink(missing_ok=True)
            args=['asset-library', self.game, output]
            for path in self.overlays: args += ['--overlay',path]
            self.command(args, progress)
            if output.stat().st_size > 128*1024*1024:
                raise ValueError('The asset library exceeds the editor memory limit')
            return json.loads(output.read_text(encoding='utf-8'))
        self.submit('index',index)

    @Slot(str)
    def report_progress(self, line):
        try:
            value=json.loads(line)
            self.progress.emit(f"Indexing installed assets: {value.get('files',0):,} / {value.get('total_files',0):,} files")
        except (ValueError, TypeError): pass

    @Slot(object, object, str)
    def completed(self, job, value, error):
        self.job = None
        if self.closed:
            self.temporary.cleanup(); return
        if error:
            self.meshFailed.emit(str(job.kind), error)
            if job.kind == 'index': self.progress.emit(error)
        elif job.kind == 'index':
            self.data=value; self.by_id={a['id']:a for a in value['items']}
            for asset in value['items']:
                for location in asset['locations']:
                    identity=location.get('resource_id')
                    if not identity: continue
                    previous=self.by_guid.get(identity)
                    self.by_guid[identity] = asset if previous is None or previous is asset else False
            self.changed.emit()
        else:
            size=value.pop('_bytes')
            value['bytes']=size
            self.cache[job.kind]=value; self.cache_bytes += size
            while len(self.cache)>16 or self.cache_bytes>48*1024*1024:
                key,item=self.cache.popitem(last=False); self.cache_bytes -= item.get('bytes',0)
            if self.wanted and self.wanted[0] == job.kind: self.meshReady.emit(job.kind,value)
        if self.wanted and self.wanted[0] not in self.cache and self.data and self.wanted[0] != job.kind:
            self.request_mesh(self.wanted[1])
        elif self.wanted and self.wanted[0] in self.cache and job.kind == 'index':
            self.meshReady.emit(self.wanted[0],self.cache[self.wanted[0]])

    def query(self, text='', category='', offset=0, limit=80):
        if not self.data:
            self.ensure(); return None
        words=text.casefold().split()
        matches=[a for a in self.data['items'] if (not category or a['category']==category) and
                 all(word in (a['name']+' '+a['native_name']+' '+a['source']).casefold() for word in words)]
        return {'ready':True,'total':len(matches),'categories':self.data['categories'],
                'errors':self.data['errors'],'items':[{**a,'_offline':True} for a in matches[offset:offset+limit]]}

    def identity(self, asset):
        return asset.get('model_resource') or next((p['resource_id'] for p in asset.get('locations',[])
                                                    if p.get('resource_id')), asset.get('id',''))

    def request_mesh(self, asset):
        identity=self.identity(asset); self.wanted=(identity,asset)
        if identity in self.cache:
            value=self.cache[identity]
            if all(self.stamp(path)==(size,mtime) for path,size,mtime in value.get('stamps',[])):
                self.cache.move_to_end(identity); self.meshReady.emit(identity,value); return
            self.cache_bytes-=self.cache.pop(identity)['bytes']
        if not self.data: self.ensure(); return
        if self.job or self.closed: return
        self.submit(identity, lambda _: self.read_mesh(asset))

    def location(self, asset, identity=None):
        for location in asset.get('locations',[]):
            if location.get('container') and location.get('resource_id'):
                if identity and location['resource_id'] != identity: continue
                relative=Path(location['stream'])
                if relative.is_absolute() or '..' in relative.parts: continue
                for root in self.overlays+[self.game]:
                    path=root/relative
                    if path.is_file(): return path, location['entry'], location['resource_id']
        raise ValueError('The original native resource is not available in this installation')

    def export(self, kind, asset, identity=None):
        self.serial += 1
        output=self.folder/(str(self.serial)+'.json')
        path,entry,identity=self.location(asset,identity)
        self.command([kind+'-export',path,entry,identity,output])
        size=output.stat().st_size
        try:
            if size>64*1024*1024: raise ValueError('This resource exceeds the preview memory limit')
            value=json.loads(output.read_text(encoding='utf-8'))
        finally: output.unlink(missing_ok=True)
        return value,size

    def read_mesh(self, asset):
        identity=self.identity(asset)
        stamps={}
        def remember(source,identity):
            path,_,_=self.location(source,identity)
            stamps[str(path)]=self.stamp(path)
            if stamps[str(path)] is None:raise ValueError('The preview source is no longer available')
        source=self.by_guid.get(identity) or self.by_id.get(asset.get('id'))
        if not source: raise ValueError('The model identity is missing or ambiguous in the offline index')
        if source['native_class']=='CHA':
            variants=[self.data['objects'][i]['model_resource'] for i in source.get('objects',[])]
            identity=next((guid for guid in variants if self.by_guid.get(guid)), None)
            source=self.by_guid.get(identity)
            if not source: raise ValueError('No original model definition is linked to this character bank')
        if source['native_class']=='MetaModel':
            remember(source,identity)
            model,_=self.export('model',source,identity)
            references={r['id']:r['resource_id'] for r in model['references']}
            # The first declared render ModelPart supplies the high-detail
            # geometry preview. Collision and shadow references are not meshes.
            fields=[f for f in model['fields'] if f['owner']=='ModelPart' and f['name']=='Model' and f['kind']=='resource']
            identity=next((references.get(f['value']) for f in fields
                           if self.by_guid.get(references.get(f['value']))),None)
            source=self.by_guid.get(identity)
            if not source: raise ValueError('This model has no unambiguous render-mesh reference')
        if source['native_class']!='EARS_MESH':
            raise ValueError('This type needs a dedicated preview adapter. Open the resource help for supported formats.')
        remember(source,identity)
        value,size=self.export('mesh',source,identity)
        geometries=[g for g in value['geometries'] if g['positions'] and g['triangles']]
        if not geometries:
            reasons=sorted({g.get('reason','Unsupported vertex declaration') for g in value['geometries']})
            raise ValueError('Native mesh preview unavailable: '+'; '.join(reasons))
        result={'name':asset['name'],'resource_id':value['resource_id'],'source_sha256':value['source_sha256'],'geometries':geometries,
                'source':value['source'],'notes':'Geometry preview · bind pose · native materials and effects in Play'}
        result['buffers']=mesh_buffers(result)
        if any(self.stamp(path)!=stamp for path,stamp in stamps.items()):
            raise ValueError('The source changed while reading its geometry; refresh the asset preview')
        result['stamps']=[(path,*stamp) for path,stamp in stamps.items()]
        result['_bytes']=size+len(result['buffers'][0])+len(result['buffers'][1])
        return result

    @staticmethod
    def stamp(path):
        try:
            info=Path(path).stat();return info.st_size,info.st_mtime_ns
        except OSError:return None

    def close(self):
        self.closed=True; self.wanted=None
        with self.lock:
            if self.process and self.process.poll() is None: self.process.terminate()
        if not self.job: self.temporary.cleanup()

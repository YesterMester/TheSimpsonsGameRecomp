"""Interactive geometry previews from decoded native vertex/index streams."""
import math

from PySide6.QtCore import QElapsedTimer, QPoint, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QCursor, QMatrix4x4, QPainter, QVector3D, QSurfaceFormat
from PySide6.QtOpenGL import QOpenGLBuffer, QOpenGLShader, QOpenGLShaderProgram, QOpenGLVertexArrayObject
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtWidgets import QCheckBox, QFrame, QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget
from shiboken6 import VoidPtr
if __package__:
    from .modkit_geometry import mesh_buffers
else:
    from modkit_geometry import mesh_buffers

VERTEX = '''#version 330 core
in vec3 position;
in vec3 normal;
uniform mat4 mvp;
out vec3 N;
out vec3 P;
void main() { N=normal; P=position; gl_Position=mvp*vec4(position,1.0); }
'''
FRAGMENT = '''#version 330 core
in vec3 N;
in vec3 P;
uniform vec3 baseColor;
out vec4 outputColor;
void main() {
  vec3 n=normalize(N);
  if(!gl_FrontFacing) n=-n;
  float key=max(dot(n,normalize(vec3(-.5,.9,.5))),0.0);
  float fill=max(dot(n,normalize(vec3(.8,.2,-.6))),0.0);
  vec3 color=baseColor*(.28+.64*key+.2*fill);
  outputColor=vec4(pow(color,vec3(1.0/2.2)),1.0);
}
'''




class MeshView(QOpenGLWidget):
    failed = Signal(str)

    def __init__(self,parent=None,spinning=False):
        super().__init__(parent)
        format=QSurfaceFormat();format.setVersion(3,3);format.setProfile(QSurfaceFormat.CoreProfile)
        format.setDepthBufferSize(24);format.setSamples(4);self.setFormat(format)
        self.setMinimumSize(180,150);self.setFocusPolicy(Qt.StrongFocus)
        self.program=self.vao=self.vbo=self.ibo=None
        self.pending=None;self.count=0;self.center=QVector3D();self.radius=1.;self.distance=3.
        self.home_center=QVector3D()
        self.yaw=35.;self.pitch=20.;self.last=None;self.spinning=spinning
        self.error='';self.title='Choose an asset to preview its native geometry';self.stats=''
        self.clock=QElapsedTimer();self.clock.start()
        self.timer=QTimer(self);self.timer.setInterval(33);self.timer.timeout.connect(self.animate)

    def initializeGL(self):
        functions=self.context().functions();functions.initializeOpenGLFunctions()
        self.program=QOpenGLShaderProgram(self)
        if not self.program.addShaderFromSourceCode(QOpenGLShader.Vertex,VERTEX) or not self.program.addShaderFromSourceCode(QOpenGLShader.Fragment,FRAGMENT) or not self.program.link():
            self.error=self.program.log();self.failed.emit(self.error);return
        self.vao=QOpenGLVertexArrayObject(self);self.vao.create()
        self.vbo=QOpenGLBuffer(QOpenGLBuffer.VertexBuffer);self.vbo.create()
        self.ibo=QOpenGLBuffer(QOpenGLBuffer.IndexBuffer);self.ibo.create()
        self.context().aboutToBeDestroyed.connect(self.release_gpu,Qt.DirectConnection)

    def release_gpu(self):
        self.makeCurrent()
        for buffer in (self.vbo,self.ibo,self.vao):
            if buffer:buffer.destroy()
        self.vbo=self.ibo=self.vao=None;self.program=None;self.doneCurrent()

    def set_mesh(self,document,buffers=None):
        self.pending=buffers or document.get('buffers') or mesh_buffers(document)
        self.title=document['name'];self.stats=f"{len(self.pending[0])//24:,} vertices · {self.pending[2]//3:,} triangles"
        self.error='';self.center=QVector3D(*self.pending[3]);self.radius=self.pending[4]
        self.home_center=QVector3D(self.center)
        self.fit();self.update()

    def fit(self):
        self.center=QVector3D(self.home_center);self.distance=self.radius*3.2;self.pitch=20.;self.update()

    def paintGL(self):
        f=self.context().functions()
        f.glClearColor(.055,.071,.095,1.);f.glClear(0x00004000|0x00000100)
        if self.program and self.program.isLinked() and self.vao:
            if self.pending:
                vertices,indices,self.count,_,_=self.pending;self.pending=None
                self.vao.bind();self.vbo.bind();self.vbo.allocate(vertices,len(vertices));self.ibo.bind();self.ibo.allocate(indices,len(indices))
                self.program.bind()
                for name,offset in [('position',0),('normal',12)]:
                    location=self.program.attributeLocation(name);self.program.enableAttributeArray(location)
                    self.program.setAttributeBuffer(location,0x1406,offset,3,24)
                self.program.release();self.vao.release()
            if self.count:
                f.glEnable(0x0B71);f.glDisable(0x0B44)
                projection=QMatrix4x4();projection.perspective(45.,max(.01,self.width()/max(1,self.height())),max(.001,self.radius*.001),self.distance+self.radius*8)
                pitch,yaw=math.radians(self.pitch),math.radians(self.yaw)
                eye=self.center+QVector3D(math.cos(pitch)*math.sin(yaw),math.sin(pitch),math.cos(pitch)*math.cos(yaw))*self.distance
                view=QMatrix4x4();view.lookAt(eye,self.center,QVector3D(0,1,0))
                self.program.bind();self.program.setUniformValue('mvp',projection*view)
                self.program.setUniformValue('baseColor',QVector3D(.35,.55,.72));self.vao.bind()
                f.glDrawElements(0x0004,self.count,0x1405,VoidPtr(0))
                self.vao.release();self.program.release();f.glDisable(0x0B71)
        painter=QPainter(self);painter.setPen(QColor('#b7c8dc'))
        if self.error or not self.count:
            painter.drawText(self.rect().adjusted(16,20,-16,-20),Qt.AlignCenter|Qt.TextWordWrap,self.error or self.title)
        else:
            painter.drawText(self.rect().adjusted(12,8,-12,-8),Qt.AlignLeft|Qt.AlignBottom,self.stats)
        painter.end()

    def animate(self):
        elapsed=min(.1,self.clock.restart()/1000)
        if self.spinning and self.count and self.last is None:self.yaw=(self.yaw+elapsed*25)%360;self.update()

    def showEvent(self,event):
        super().showEvent(event);self.clock.restart();self.timer.start()

    def hideEvent(self,event):
        self.timer.stop();super().hideEvent(event)

    def mousePressEvent(self,event):
        self.last=event.position();self.setFocus();event.accept()

    def mouseMoveEvent(self,event):
        if self.last is None:return
        delta=event.position()-self.last;self.last=event.position()
        if event.buttons() & Qt.LeftButton:
            self.yaw-=delta.x()*.45;self.pitch=max(-89,min(89,self.pitch+delta.y()*.45))
        elif event.buttons() & (Qt.MiddleButton|Qt.RightButton):
            step=self.distance/max(1,self.height())*.8;yaw=math.radians(self.yaw)
            self.center+=QVector3D(-math.cos(yaw)*delta.x()*step,delta.y()*step,math.sin(yaw)*delta.x()*step)
        self.update();event.accept()

    def mouseReleaseEvent(self,event):
        self.last=None;event.accept()

    def wheelEvent(self,event):
        self.distance=max(self.radius*.03,min(self.radius*100,self.distance*math.exp(-event.angleDelta().y()/900)))
        self.update();event.accept()

    def keyPressEvent(self,event):
        if event.key()==Qt.Key_F:self.fit();event.accept()
        else:super().keyPressEvent(event)


class HoverModel(QFrame):
    def __init__(self,parent=None):
        super().__init__(parent,Qt.ToolTip)
        self.setAttribute(Qt.WA_ShowWithoutActivating);self.setAttribute(Qt.WA_TransparentForMouseEvents)
        self.setObjectName('assetHoverPreview');self.setFixedSize(320,290)
        layout=QVBoxLayout(self);layout.setContentsMargins(1,1,1,1)
        self.label=QLabel();self.label.setMargin(8);self.label.setTextFormat(Qt.PlainText);layout.addWidget(self.label)
        self.view=MeshView(spinning=True);layout.addWidget(self.view,1)
        hint=QLabel('Native geometry · bind pose');hint.setMargin(6);layout.addWidget(hint)

    def open_for(self,asset):
        self.label.setText(asset['name']);self.view.count=0;self.view.pending=None
        self.view.title='Reading native model…';self.view.error=''
        position=QCursor.pos()+QPoint(20,20);screen=self.screen().availableGeometry()
        position.setX(min(position.x(),screen.right()-self.width()));position.setY(min(position.y(),screen.bottom()-self.height()))
        self.move(position);self.show();self.view.update()


class ModelPreview(QWidget):
    helpRequested = Signal()

    def __init__(self,parent=None):
        super().__init__(parent);self.identity=None
        layout=QVBoxLayout(self);layout.setContentsMargins(0,0,0,0)
        row=QHBoxLayout();self.label=QLabel('Select a model in the asset library');self.label.setWordWrap(True);row.addWidget(self.label,1)
        spin=QCheckBox('Spin');spin.setChecked(True);row.addWidget(spin)
        fit=QPushButton('Frame · F');row.addWidget(fit)
        help=QPushButton('Resource help');help.clicked.connect(self.helpRequested);row.addWidget(help)
        layout.addLayout(row)
        self.view=MeshView(spinning=True);layout.addWidget(self.view,1)
        fit.clicked.connect(self.view.fit);spin.toggled.connect(lambda enabled:setattr(self.view,'spinning',enabled))
        self.note=QLabel('Orbit: drag left · Pan: drag right/middle · Zoom: wheel · Frame: F')
        self.note.setWordWrap(True);layout.addWidget(self.note)

    def loading(self,asset,identity):
        self.identity=identity;self.label.setText(asset['name']);self.view.count=0;self.view.pending=None
        self.view.error='';self.view.title='Reading original model geometry…';self.view.update()

    def accept(self,document):
        self.label.setText(document['name']);self.note.setText(document['notes']+'\nOrbit: left drag · Pan: right/middle drag · Zoom: wheel · Frame: F')
        self.view.set_mesh(document)

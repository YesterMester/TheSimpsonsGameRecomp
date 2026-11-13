// Guided metadata editing shares the source editor's revision guard and Save.
// Native menus stamp world points; this form edits their names and rules.
(() => {
  let raw = false, selected = "", baseRevision = 1;
  const el = id => document.getElementById(id);
  const read = () => JSON.parse(el("projectsource").value);
  const point = value => value.mission.objectives.find(p => p.id === selected);
  const numeric = input => {
    const value = input.valueAsNumber;
    if (!Number.isFinite(value)) throw new Error("Enter a finite number before saving.");
    return value;
  };
  function edited(change, contract = true, render = false) {
    try {
      const value = read();
      change(value.mission, point(value));
      if (contract) value.mission.revision = Math.min(4294967295, baseRevision + 1);
      el("projectsource").value = JSON.stringify(value, null, 2) + "\n";
      el("projectsource").dispatchEvent(new Event("input"));
      if (render) window.renderMissionProject(false);
    } catch (error) { el("projectstatus").textContent = error.message; }
  }
  function visibility(objective) {
    document.querySelectorAll(".missionposition").forEach(node => node.classList.toggle("hidden", !objective || objective.kind === "wait"));
    el("missiondurationfield").classList.toggle("hidden", !objective || objective.kind === "reach");
  }
  window.renderMissionProject = reset => {
    const candidate = creatorSource && creatorSource.path === "mod.json";
    let value;
    try { value = candidate ? read() : null; } catch (_) { value = null; }
    const has = !!(value && value.mission && Array.isArray(value.mission.objectives));
    if (reset) raw = false;
    el("missionproperties").classList.toggle("hidden", !has || raw);
    el("missionraw").classList.toggle("hidden", !has);
    el("missionraw").textContent = raw ? "Show mission settings" : "Edit JSON source";
    el("projectsource").classList.toggle("hidden", has && !raw);
    el("projectsourcehint").classList.toggle("hidden", has && !raw);
    el("projectsave").textContent = has && !raw ? "Save mission settings" : "Save source";
    if (!has || raw) return;
    const doc = value.mission;
    // One form edit session increments the contract once. Re-rendering a
    // selector or reordering rows must not bump it again on every keystroke.
    try { baseRevision = Number(JSON.parse(creatorSource.text).mission.revision || 1); }
    catch (_) { baseRevision = Number(doc.revision || 1); }
    el("missionname").value = doc.name || "";
    el("missiondescription").value = doc.description || "";
    el("missionepisode").value = doc.level_root || "Place the first point in Test";
    el("missionlimit").value = doc.time_limit || 0;
    el("missionauto").checked = !!doc.auto_start;
    el("missiondefeat").checked = doc.fail_on_defeat !== false;
    if (!doc.objectives.some(p => p.id === selected)) selected = doc.objectives[0] ? doc.objectives[0].id : "";
    const selector = el("missionobjective"); selector.textContent = "";
    for (const [index, object] of doc.objectives.entries()) {
      const option = document.createElement("option"); option.value = object.id;
      option.textContent = `${index + 1}. ${object.title}`; selector.appendChild(option);
    }
    selector.value = selected; syncSelectLabel(selector);
    const object = point(value);
    el("missionempty").classList.toggle("hidden", !!object);
    el("missionobjectivefields").classList.toggle("hidden", !object);
    if (!object) return;
    el("missionobjectiveid").textContent = `Stable id: ${object.id}. Keeping this id preserves prerequisite links.`;
    el("missionpointtitle").value = object.title;
    el("missionkind").value = object.kind; syncSelectLabel(el("missionkind"));
    for (const axis of ["x", "y", "z"]) el("mission" + axis).value = (object.position || {})[axis] || 0;
    el("missionradius").value = object.radius === undefined ? 3 : object.radius;
    el("missionseconds").value = object.seconds === undefined ? 2 : object.seconds;
    el("missionoptional").checked = !!object.optional;
    el("missioncheckpoint").checked = !!object.checkpoint;
    const parents = el("missionparents"); parents.textContent = "";
    for (const other of doc.objectives) {
      if (other.id === object.id) continue;
      const label = document.createElement("label"), checkbox = document.createElement("input");
      checkbox.type = "checkbox"; checkbox.checked = (object.requires || []).includes(other.id);
      checkbox.addEventListener("change", () => edited((doc, current) => {
        const required = new Set(current.requires || []);
        if (checkbox.checked) required.add(other.id); else required.delete(other.id);
        current.requires = [...required].sort();
      }));
      label.append(checkbox, document.createTextNode(" " + other.title)); parents.appendChild(label);
    }
    if (!parents.childNodes.length) parents.textContent = "No other objectives yet.";
    visibility(object);
    creatorControls();
  };
  el("projectmission").onclick = () => {
    if (!discardCreatorChanges()) return;
    creatorWork(() => loadProjectSource("mod.json"));
  };
  el("missionraw").onclick = () => { raw = !raw; window.renderMissionProject(false); };
  el("missionobjective").addEventListener("change", () => { selected = el("missionobjective").value; window.renderMissionProject(false); });
  for (const [node, field] of [["missionname", "name"], ["missiondescription", "description"]])
    el(node).addEventListener("input", () => edited(doc => doc[field] = el(node).value, false));
  el("missionlimit").addEventListener("input", () => edited(doc => doc.time_limit = numeric(el("missionlimit"))));
  el("missionauto").addEventListener("change", () => edited(doc => doc.auto_start = el("missionauto").checked));
  el("missiondefeat").addEventListener("change", () => edited(doc => doc.fail_on_defeat = el("missiondefeat").checked));
  el("missionpointtitle").addEventListener("input", () => edited((doc, p) => p.title = el("missionpointtitle").value, false));
  el("missionkind").addEventListener("change", () => edited((doc, p) => {
    p.kind = el("missionkind").value;
    p.position = p.position || {x: 0, y: 0, z: 0};
    p.radius = p.radius === undefined ? 3 : p.radius;
    p.seconds = p.seconds === undefined ? 2 : p.seconds;
  }, true, true));
  for (const axis of ["x", "y", "z"]) el("mission" + axis).addEventListener("input", () => edited((doc, p) => {
    p.position = p.position || {x: 0, y: 0, z: 0}; p.position[axis] = numeric(el("mission" + axis));
  }));
  for (const [node, field] of [["missionradius", "radius"], ["missionseconds", "seconds"]])
    el(node).addEventListener("input", () => edited((doc, p) => p[field] = numeric(el(node))));
  for (const [node, field] of [["missionoptional", "optional"], ["missioncheckpoint", "checkpoint"]])
    el(node).addEventListener("change", () => edited((doc, p) => p[field] = el(node).checked));
  for (const [node, offset] of [["missionup", -1], ["missiondown", 1]]) el(node).onclick = () => edited(doc => {
    const index = doc.objectives.findIndex(p => p.id === selected), to = index + offset;
    if (to >= 0 && to < doc.objectives.length) [doc.objectives[index], doc.objectives[to]] = [doc.objectives[to], doc.objectives[index]];
  }, false, true);
  el("missionremove").onclick = () => {
    if (!confirm("Remove this objective and its prerequisite links?")) return;
    edited(doc => {
      doc.objectives = doc.objectives.filter(p => p.id !== selected);
      for (const object of doc.objectives) object.requires = (object.requires || []).filter(id => id !== selected);
      selected = "";
    }, true, true);
  };
})();

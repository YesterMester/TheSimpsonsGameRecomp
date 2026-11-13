package io.github.yestermester.simpsons;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Control schemes and their saved layouts. Layout data is separate from shared
 * preferences so both app processes see edits. The file keeps one layout per
 * scheme; version 1 files held the touch gamepad's layout alone.
 */
final class TouchLayoutStore {
    static final String OFF = "off";

    static JSONObject schema(Context context) throws Exception {
        return new JSONObject(readAsset(context, "touch-controls.json"));
    }
    static String readAsset(Context context, String name) throws IOException {
        try (java.io.InputStream input = context.getAssets().open(name); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] bytes = new byte[8192]; int count;
            while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    /**
     * The chosen scheme, "off" for physical controls only. It is kept in the
     * layout file too: the game runs in its own process, whose cached
     * preferences never see a choice made in the editor or the launcher.
     */
    static String activeScheme(Context context) {
        String scheme = loadAll(context).optString("active", "");
        if (scheme.isEmpty()) scheme = context.getSharedPreferences("settings", Context.MODE_PRIVATE).getString("touch_scheme", "");
        if (OFF.equals(scheme)) return OFF;
        try {
            JSONObject root = schema(context);
            return root.getJSONObject("schemes").has(scheme) ? scheme : root.getString("default_scheme");
        } catch (Exception error) { return "simple"; }
    }
    /** A scheme to edit or draw: the chosen one, or the default when touch controls are off. */
    static String editableScheme(Context context, String requested) {
        try {
            JSONObject root = schema(context);
            if (requested != null && root.getJSONObject("schemes").has(requested)) return requested;
            String active = activeScheme(context);
            return OFF.equals(active) ? root.getString("default_scheme") : active;
        } catch (Exception error) { return "simple"; }
    }
    static JSONObject scheme(Context context, String id) throws Exception {
        return schema(context).getJSONObject("schemes").getJSONObject(editableScheme(context, id));
    }
    /** The schemes as [{id, label}], for pickers. */
    static JSONArray schemeList(Context context) throws Exception {
        JSONObject schemes = schema(context).getJSONObject("schemes");
        JSONArray list = new JSONArray();
        for (Iterator<String> keys = schemes.keys(); keys.hasNext();) {
            String id = keys.next();
            list.put(new JSONObject().put("id", id).put("label", schemes.getJSONObject(id).getString("label")));
        }
        return list;
    }
    /** Chooses a scheme, keeping the launcher's copy of the settings in step. */
    static synchronized void setActiveScheme(Context context, String scheme) throws Exception {
        if (!OFF.equals(scheme) && !schema(context).getJSONObject("schemes").has(scheme)) {
            throw new IllegalArgumentException("Unknown control scheme");
        }
        JSONObject all = loadAll(context);
        all.put("version", 2).put("active", scheme);
        write(context, all);
        SharedPreferences preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE);
        JSONObject values = new JSONObject(preferences.getString("launcher_values", "{}"));
        values.put("touch_scheme", scheme);
        preferences.edit().putString("touch_scheme", scheme).putString("launcher_values", values.toString()).commit();
    }

    private static AtomicFile file(Context context) {
        return new AtomicFile(new File(context.getFilesDir(), "touch-layout.json"));
    }
    private static JSONObject loadAll(Context context) {
        try {
            JSONObject saved = new JSONObject(new String(file(context).readFully(), StandardCharsets.UTF_8));
            if (saved.optInt("version", 1) == 1) {
                JSONObject controls = saved.optJSONObject("controls");
                saved = new JSONObject().put("version", 2).put("schemes", new JSONObject()
                        .put("gamepad", new JSONObject().put("controls", controls == null ? new JSONObject() : controls)));
            }
            if (saved.optJSONObject("schemes") == null) saved.put("schemes", new JSONObject());
            return saved;
        } catch (FileNotFoundException absent) {
        } catch (Exception error) {
            android.util.Log.w("TouchControls", "Could not load the custom layouts; using defaults", error);
        }
        try { return new JSONObject().put("version", 2).put("schemes", new JSONObject()); }
        catch (Exception impossible) { return new JSONObject(); }
    }
    /** The saved layout of one scheme, {"controls": {id: {x, y, size, opacity, hidden}}}. */
    static JSONObject load(Context context, String scheme) {
        try {
            JSONObject saved = loadAll(context).getJSONObject("schemes").optJSONObject(editableScheme(context, scheme));
            return validate(context, editableScheme(context, scheme), saved == null ? "{}" : saved.toString());
        } catch (Exception error) {
            android.util.Log.w("TouchControls", "Could not load the custom layout; using defaults", error);
            return new JSONObject();
        }
    }
    private static double number(JSONObject value, String key, double min, double max) throws Exception {
        double number = value.getDouble(key);
        if (!Double.isFinite(number)) throw new IllegalArgumentException("Invalid control " + key);
        return Math.max(min, Math.min(max, number));
    }
    static JSONObject validate(Context context, String scheme, String text) throws Exception {
        if (text.length() > 32768) throw new IllegalArgumentException("The touch layout is too large");
        JSONObject input = new JSONObject(text), controls = new JSONObject();
        JSONObject incoming = input.optJSONObject("controls");
        JSONArray definitions = scheme(context, scheme).getJSONArray("controls");
        for (int i = 0; incoming != null && i < definitions.length(); i++) {
            String id = definitions.getJSONObject(i).getString("id");
            if (!incoming.has(id)) continue;
            JSONObject source = incoming.getJSONObject(id), value = new JSONObject();
            if (source.has("x")) value.put("x", number(source, "x", 0, 1));
            if (source.has("y")) value.put("y", number(source, "y", 0, 1));
            if (source.has("size")) value.put("size", number(source, "size", .55, 1.8));
            if (source.has("opacity")) value.put("opacity", number(source, "opacity", .1, 1));
            if (source.has("hidden")) value.put("hidden", source.getBoolean("hidden"));
            controls.put(id, value);
        }
        return new JSONObject().put("controls", controls);
    }
    static synchronized void save(Context context, String scheme, String text) throws Exception {
        String id = editableScheme(context, scheme);
        JSONObject all = loadAll(context);
        all.put("version", 2);
        all.getJSONObject("schemes").put(id, validate(context, id, text));
        write(context, all);
    }
    private static void write(Context context, JSONObject all) throws Exception {
        byte[] bytes = all.toString().getBytes(StandardCharsets.UTF_8);
        AtomicFile target = file(context);
        FileOutputStream output = target.startWrite();
        try { output.write(bytes); target.finishWrite(output); }
        catch (Exception error) { target.failWrite(output); throw error; }
    }
}

package io.github.yestermester.simpsons;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.SparseArray;
import android.view.Choreographer;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import java.util.ArrayList;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Touch controls in the chosen scheme. Simple touch moves with a stick that
 * appears under the thumb anywhere in the left zone, looks around with swipes
 * in the right zone and names its actions; the touch gamepad has every stick
 * and button of a controller. Each finger owns its control until released.
 */
final class TouchControlsView extends View {
    static final int STICK_LEFT = 1, STICK_RIGHT = 2, DPAD = 3;
    static final int BUTTON = 4, TRIGGER_LEFT = 5, TRIGGER_RIGHT = 6;
    static final int MOVE_AREA = 7, LOOK_AREA = 8, MENU = 9;
    // A swipe this fast (in layout units per second) turns the camera at the
    // right stick's full speed, before the camera sensitivity. Slower swipes
    // follow a curve that keeps small adjustments responsive.
    private static final float LOOK_FULL_SPEED = 420f;
    // The camera stick turns at full speed from this fraction of its throw.
    private static final float CAMERA_STICK_REACH = .7f;

    interface Listener { void onOpenMenu(); }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<Control> controls = new ArrayList<>();
    private final SparseArray<Finger> fingers = new SparseArray<>();
    private final RectF bounds = new RectF();
    private final Path gear = new Path();
    private final String scheme;
    private final boolean floatingStick, haptics, showMenu;
    private final float controlScale, opacity;
    private final float deadzone, cameraSensitivity;
    private final boolean cameraInvertY;
    private final JSONArray definitions;
    private Listener listener;
    private float leftInset, rightInset, topInset, bottomInset;
    private float unit;
    private int lastButtons, lastLt, lastRt, lastLx, lastLy, lastRx, lastRy;
    private boolean lookFrameScheduled;
    private long lookFrameNanos;

    private static final class Control {
        final int kind, button, color;
        final String label;
        final float x, y, radius, opacity, scale;
        final RectF area;
        Control(int kind, int button, String label, float x, float y, float radius, float opacity,
                float scale, int color, RectF area) {
            this.kind = kind;
            this.button = button;
            this.label = label;
            this.x = x;
            this.y = y;
            this.radius = radius;
            this.opacity = opacity;
            this.scale = scale;
            this.color = color;
            this.area = area;
        }
        boolean isStick() { return kind == STICK_LEFT || kind == STICK_RIGHT || kind == MOVE_AREA; }
    }

    private static final class Finger {
        Control control;
        float originX, originY, x, y;
        float axisX, axisY;
        // Swipe to look: movement since the last frame.
        float pendingX, pendingY, lastX, lastY;
        Finger(Control control, float originX, float originY, float x, float y) {
            this.control = control;
            this.originX = originX;
            this.originY = originY;
            this.x = x;
            this.y = y;
            this.lastX = x;
            this.lastY = y;
        }
    }

    TouchControlsView(Context context) {
        super(context);
        SharedPreferences prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE);
        controlScale = Math.max(0.8f, Math.min(1.3f, prefs.getFloat("touch_size", 1f)));
        opacity = Math.max(0.2f, Math.min(0.85f, prefs.getFloat("touch_opacity", 0.5f)));
        floatingStick = prefs.getBoolean("floating_stick", true);
        haptics = prefs.getBoolean("touch_haptics", true);
        showMenu = prefs.getBoolean("touch_show_editor", true);
        deadzone = Math.max(0, Math.min(.35f, prefs.getFloat("touch_deadzone", .12f)));
        cameraSensitivity = Math.max(.5f, Math.min(2, prefs.getFloat("touch_camera_sensitivity", 1)));
        cameraInvertY = prefs.getBoolean("touch_camera_invert_y", false);
        scheme = TouchLayoutStore.editableScheme(context, null);
        JSONArray loaded;
        try { loaded = TouchLayoutStore.scheme(context, scheme).getJSONArray("controls"); }
        catch (Exception error) { android.util.Log.e("TouchControls", "Packaged control definitions could not be read", error); loaded = new JSONArray(); }
        definitions = loaded;
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        setFocusable(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(
                        WindowInsets.Type.displayCutout() | WindowInsets.Type.systemGestures());
                leftInset = safe.left;
                rightInset = safe.right;
                topInset = safe.top;
                bottomInset = safe.bottom;
            } else {
                android.view.DisplayCutout cutout = insets.getDisplayCutout();
                leftInset = cutout == null ? 0 : cutout.getSafeInsetLeft();
                rightInset = cutout == null ? 0 : cutout.getSafeInsetRight();
                topInset = cutout == null ? 0 : cutout.getSafeInsetTop();
                bottomInset = cutout == null ? 0 : cutout.getSafeInsetBottom();
            }
            layoutControls();
            return insets;
        });
    }

    void setListener(Listener listener) { this.listener = listener; }

    @Override protected void onSizeChanged(int w, int h, int oldW, int oldH) {
        layoutControls();
    }

    private static int parseColor(String value) {
        if (value == null || value.isEmpty()) return 0;
        try { return Color.parseColor(value); }
        catch (IllegalArgumentException error) { return 0; }
    }

    private void layoutControls() {
        reset();
        controls.clear();
        float width = getWidth() - leftInset - rightInset;
        float height = getHeight() - topInset - bottomInset;
        if (width <= 0 || height <= 0) return;
        unit = Math.min(width / 860f, height / 440f) * controlScale;
        float margin = 12 * unit;
        bounds.set(leftInset + margin, topInset + margin,
                getWidth() - rightInset - margin, getHeight() - bottomInset - margin);
        JSONObject custom = TouchLayoutStore.load(getContext(), scheme).optJSONObject("controls");
        try {
            for (int i = 0; i < definitions.length(); i++) {
                JSONObject definition = definitions.getJSONObject(i);
                JSONObject value = custom == null ? null : custom.optJSONObject(definition.getString("id"));
                if (value == null) value = new JSONObject();
                int kind = definition.getInt("kind");
                if (value.optBoolean("hidden", definition.optBoolean("hidden"))) continue;
                if (kind == MENU && !showMenu) continue;
                float scale = (float) value.optDouble("size", 1);
                float radius = Math.min((float) definition.getDouble("radius") * unit * scale,
                        Math.min(bounds.width(), bounds.height()) * .45f);
                float x = bounds.left + (float) value.optDouble("x", definition.getDouble("x")
                        + definition.optDouble("dx", 0) * unit / bounds.width()) * bounds.width();
                float y = bounds.top + (float) value.optDouble("y", definition.getDouble("y")
                        + definition.optDouble("dy", 0) * unit / bounds.height()) * bounds.height();
                RectF area = null;
                JSONObject zone = definition.optJSONObject("area");
                if (zone != null) {
                    // Zones reach the screen edges; the insets keep gesture
                    // edges free.
                    area = new RectF(
                            leftInset + (float) zone.getDouble("left") * width,
                            topInset + (float) zone.getDouble("top") * height,
                            leftInset + (float) zone.getDouble("right") * width,
                            topInset + (float) zone.getDouble("bottom") * height);
                }
                boolean stick = kind == STICK_LEFT || kind == STICK_RIGHT || kind == MOVE_AREA;
                float labelSpace = stick ? 18 * unit : 0;
                controls.add(new Control(kind, definition.getInt("button"), definition.getString("label"),
                        Math.max(bounds.left + radius, Math.min(bounds.right - radius, x)),
                        Math.max(bounds.top + radius, Math.min(bounds.bottom - radius - labelSpace, y)),
                        radius, (float) value.optDouble("opacity", opacity), scale,
                        parseColor(definition.optString("color")), area));
            }
        } catch (Exception error) { android.util.Log.e("TouchControls", "Could not lay out controls", error); }
        buildGear();
        invalidate();
    }
    void reloadLayout() { layoutControls(); }

    // A unit gear: eight teeth around a ring, scaled when drawn.
    private void buildGear() {
        gear.reset();
        int teeth = 8;
        for (int i = 0; i < teeth * 2; i++) {
            double a0 = i * Math.PI / teeth, a1 = (i + 1) * Math.PI / teeth;
            float radius = i % 2 == 0 ? .62f : .46f;
            float x0 = (float) Math.cos(a0) * radius, y0 = (float) Math.sin(a0) * radius;
            float x1 = (float) Math.cos(a1) * radius, y1 = (float) Math.sin(a1) * radius;
            if (i == 0) gear.moveTo(x0, y0); else gear.lineTo(x0, y0);
            gear.lineTo(x1, y1);
        }
        gear.close();
        gear.addCircle(0, 0, .2f, Path.Direction.CCW);
        gear.setFillType(Path.FillType.EVEN_ODD);
    }

    private Control hit(float x, float y) {
        Control best = null;
        float nearest = Float.MAX_VALUE;
        for (Control control : controls) {
            if (control.kind == MOVE_AREA || control.kind == LOOK_AREA) continue;
            float dx = x - control.x, dy = y - control.y;
            float distance = (dx * dx + dy * dy) / (control.radius * control.radius);
            if (distance <= 1.35f && distance < nearest) {
                best = control;
                nearest = distance;
            }
        }
        if (best != null) return best;
        // Zones: moving takes precedence where they meet.
        for (Control control : controls) if (control.kind == MOVE_AREA && control.area.contains(x, y)) return control;
        for (Control control : controls) if (control.kind == LOOK_AREA && control.area.contains(x, y)) return control;
        if (floatingStick && bounds.contains(x, y)) {
            for (Control control : controls) if (control.kind == STICK_LEFT
                    && Math.abs(x - control.x) <= Math.max(control.radius * 2, bounds.width() * .18f)
                    && Math.abs(y - control.y) <= Math.max(control.radius * 1.8f, bounds.height() * .24f)) return control;
        }
        return null;
    }

    private boolean controlOccupied(Control control) {
        for (int i = 0; i < fingers.size(); ++i) {
            if (fingers.valueAt(i).control == control) return true;
        }
        return false;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            reset();
            return true;
        }
        int index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = event.getX(index), y = event.getY(index);
            Control control = hit(x, y);
            boolean exclusive = control != null && (control.isStick() || control.kind == LOOK_AREA);
            if (control != null && (!exclusive || !controlOccupied(control))) {
                float originX = control.x, originY = control.y;
                if (control.kind == MOVE_AREA || (control.kind == STICK_LEFT && floatingStick)) {
                    // The stick appears under the thumb.
                    originX = Math.max(bounds.left + control.radius,
                            Math.min(bounds.right - control.radius, x));
                    originY = Math.max(bounds.top + control.radius, Math.min(bounds.bottom - control.radius - 18 * unit, y));
                }
                fingers.put(event.getPointerId(index), new Finger(control, originX, originY, x, y));
                if (control.kind == LOOK_AREA) scheduleLookFrame();
                if (haptics && control.kind >= BUTTON && control.kind <= TRIGGER_RIGHT) {
                    performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                }
            } else if (action == MotionEvent.ACTION_DOWN) {
                // Leave touches outside the controls to the SDL surface.
                return false;
            }
        }
        for (int i = 0; i < event.getPointerCount(); ++i) {
            Finger finger = fingers.get(event.getPointerId(i));
            if (finger == null) continue;
            finger.x = event.getX(i);
            finger.y = event.getY(i);
            if (finger.control.kind == LOOK_AREA) {
                finger.pendingX += finger.x - finger.lastX;
                finger.pendingY += finger.y - finger.lastY;
                finger.lastX = finger.x;
                finger.lastY = finger.y;
            } else if (finger.control.kind == MOVE_AREA) {
                // Past the rim the stick follows the thumb, so it never runs out.
                float dx = finger.x - finger.originX, dy = finger.y - finger.originY;
                float distance = (float) Math.hypot(dx, dy), limit = finger.control.radius * 1.25f;
                if (distance > limit) {
                    finger.originX += dx * (distance - limit) / distance;
                    finger.originY += dy * (distance - limit) / distance;
                }
            } else if (action == MotionEvent.ACTION_MOVE && finger.control.kind == BUTTON) {
                Control next = hit(finger.x, finger.y);
                // Sliding from X to Y works for combos; moving off a button
                // releases it instead of holding it until the finger lifts.
                if (next != null && next.kind == BUTTON) finger.control = next;
            }
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            Finger finger = fingers.get(event.getPointerId(index));
            fingers.remove(event.getPointerId(index));
            if (finger != null && finger.control.kind == MENU && pressed(finger) && listener != null) {
                if (haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                listener.onOpenMenu();
            }
            if (action == MotionEvent.ACTION_UP) performClick();
        }
        publish();
        postInvalidateOnAnimation();
        return true;
    }

    @Override public boolean performClick() { super.performClick(); return true; }

    private void scheduleLookFrame() {
        if (lookFrameScheduled) return;
        lookFrameScheduled = true;
        lookFrameNanos = 0;
        Choreographer.getInstance().postFrameCallback(this::onLookFrame);
    }

    // Once per display frame while a finger swipes to look: the swipe's speed
    // becomes the right stick, so the camera turns as far as the finger moves
    // and stops when it rests.
    private void onLookFrame(long frameNanos) {
        lookFrameScheduled = false;
        boolean looking = false;
        float elapsed = lookFrameNanos == 0 ? 1 / 60f : Math.max(1 / 240f, Math.min(.1f, (frameNanos - lookFrameNanos) * 1e-9f));
        lookFrameNanos = frameNanos;
        float fullSpeed = LOOK_FULL_SPEED * unit;
        for (int i = 0; i < fingers.size(); ++i) {
            Finger finger = fingers.valueAt(i);
            if (finger.control.kind != LOOK_AREA) continue;
            looking = true;
            float targetX = lookCurve(finger.pendingX / elapsed / fullSpeed * cameraSensitivity);
            float targetY = lookCurve(-finger.pendingY / elapsed / fullSpeed * cameraSensitivity);
            finger.pendingX = finger.pendingY = 0;
            // Just enough smoothing to even out uneven touch reports.
            finger.axisX = Math.max(-1, Math.min(1, finger.axisX * .15f + targetX * .85f));
            finger.axisY = Math.max(-1, Math.min(1, finger.axisY * .15f + targetY * .85f));
            if (Math.abs(finger.axisX) < .02f) finger.axisX = 0;
            if (Math.abs(finger.axisY) < .02f) finger.axisY = 0;
        }
        publish();
        if (looking) {
            lookFrameScheduled = true;
            Choreographer.getInstance().postFrameCallback(this::onLookFrame);
        }
    }

    // Raises small values (|value| < 1) so slow swipes still turn the camera.
    private static float lookCurve(float value) {
        float magnitude = Math.min(1f, Math.abs(value));
        return Math.copySign((float) Math.pow(magnitude, .75), value);
    }

    private void axes(Finger finger) {
        float x = (finger.x - finger.originX) / finger.control.radius;
        float y = (finger.originY - finger.y) / finger.control.radius;
        float length = (float) Math.hypot(x, y);
        if (length <= deadzone) { finger.axisX = finger.axisY = 0; return; }
        boolean camera = finger.control.kind == STICK_RIGHT;
        float sensitivity = camera ? cameraSensitivity : 1;
        float reach = camera ? Math.max(deadzone + .1f, CAMERA_STICK_REACH) : 1;
        float scale = Math.min(1f, (length - deadzone) / (reach - deadzone)) * sensitivity / length;
        finger.axisX = Math.max(-1, Math.min(1, x * scale));
        finger.axisY = Math.max(-1, Math.min(1, y * scale));
        if (finger.control.kind == STICK_RIGHT && cameraInvertY) finger.axisY = -finger.axisY;
    }

    private boolean pressed(Finger finger) {
        float dx = finger.x - finger.control.x, dy = finger.y - finger.control.y;
        return dx * dx + dy * dy <= finger.control.radius * finger.control.radius * 1.6f;
    }

    private void publish() {
        int buttons = 0, lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
        for (int i = 0; i < fingers.size(); ++i) {
            Finger finger = fingers.valueAt(i);
            Control control = finger.control;
            if (control.isStick()) {
                axes(finger);
                if (control.kind == STICK_RIGHT) {
                    rx = Math.round(finger.axisX * 32767);
                    ry = Math.round(finger.axisY * 32767);
                } else {
                    lx = Math.round(finger.axisX * 32767);
                    ly = Math.round(finger.axisY * 32767);
                }
            } else if (control.kind == LOOK_AREA) {
                float axisY = cameraInvertY ? -finger.axisY : finger.axisY;
                rx = Math.round(finger.axisX * 32767);
                ry = Math.round(axisY * 32767);
            } else if (control.kind == DPAD) {
                float x = (finger.x - control.x) / control.radius;
                float y = (finger.y - control.y) / control.radius;
                if (Math.abs(x) > 0.28f && Math.abs(x) >= Math.abs(y) * 0.5f) buttons |= x < 0 ? 4 : 8;
                if (Math.abs(y) > 0.28f && Math.abs(y) >= Math.abs(x) * 0.5f) buttons |= y < 0 ? 1 : 2;
            } else if (control.kind != MENU && pressed(finger)) {
                if (control.kind == TRIGGER_LEFT) lt = 255;
                else if (control.kind == TRIGGER_RIGHT) rt = 255;
                else buttons |= control.button;
            }
        }
        if (buttons != lastButtons || lt != lastLt || rt != lastRt || lx != lastLx || ly != lastLy || rx != lastRx || ry != lastRy) {
            GameActivity.nativeTouchState(buttons, lt, rt, lx, ly, rx, ry);
            lastButtons = buttons;
            lastLt = lt; lastRt = rt;
            lastLx = lx; lastLy = ly; lastRx = rx; lastRy = ry;
        }
    }

    void reset() {
        fingers.clear();
        publish();
        invalidate();
    }

    @Override protected void onDetachedFromWindow() {
        reset();
        super.onDetachedFromWindow();
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        for (Control control : controls) {
            if (control.kind == LOOK_AREA) continue;
            float opacity = control.opacity;
            Finger active = null;
            for (int i = 0; i < fingers.size(); ++i) {
                Finger finger = fingers.valueAt(i);
                if (finger.control == control && (control.kind <= DPAD || control.kind == MOVE_AREA || pressed(finger))) {
                    active = finger;
                    break;
                }
            }
            boolean stick = control.isStick();
            // The move zone's stick rests faintly where the thumb usually is.
            if (control.kind == MOVE_AREA && active == null) opacity *= .55f;
            float x = active != null && stick ? active.originX : control.x;
            float y = active != null && stick ? active.originY : control.y;
            paint.setStyle(Paint.Style.FILL);
            if (control.color != 0) {
                paint.setColor(control.color);
                paint.setAlpha((int) ((active != null ? 235 : 170) * opacity));
            } else {
                paint.setColor(Color.argb((int) (120 * opacity), 12, 18, 26));
            }
            canvas.drawCircle(x, y, control.radius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth((active != null ? 2.5f : 1.5f) * unit);
            paint.setColor(active == null ? Color.argb((int) (230 * opacity), 240, 244, 248) : Color.rgb(255, 208, 56));
            canvas.drawCircle(x, y, control.radius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setTextAlign(Paint.Align.CENTER);
            int text = Color.argb((int) (Math.min(1f, opacity * 1.6f) * 255), 246, 242, 230);
            if (stick) {
                float dx = 0, dy = 0;
                if (active != null) {
                    dx = active.x - x;
                    dy = active.y - y;
                    float distance = (float) Math.hypot(dx, dy);
                    float limit = control.radius * 0.7f;
                    if (distance > limit) { dx *= limit / distance; dy *= limit / distance; }
                }
                paint.setColor(Color.argb((int) (210 * opacity), 230, 237, 243));
                canvas.drawCircle(x + dx, y + dy, control.radius * 0.38f, paint);
                paint.setColor(text);
                paint.setTextSize(10 * unit * control.scale);
                canvas.drawText(control.label, x, y + control.radius + 15 * unit, paint);
            } else if (control.kind == DPAD) {
                float r = control.radius;
                paint.setColor(text);
                paint.setTextSize(19 * unit * control.scale);
                canvas.drawText("▲", x, y - r * 0.43f, paint);
                canvas.drawText("▼", x, y + r * 0.77f, paint);
                canvas.drawText("◀", x - r * 0.60f, y + 6 * unit, paint);
                canvas.drawText("▶", x + r * 0.60f, y + 6 * unit, paint);
                if (!control.label.equals("D-PAD")) {
                    paint.setTextSize(10 * unit * control.scale);
                    canvas.drawText(control.label, x, y + r + 15 * unit, paint);
                }
            } else if (control.kind == MENU) {
                paint.setColor(text);
                canvas.save();
                canvas.translate(x, y);
                canvas.scale(control.radius, control.radius);
                canvas.drawPath(gear, paint);
                canvas.restore();
            } else {
                paint.setColor(text);
                float size = control.label.length() > 4 ? 10 : control.label.length() > 2 ? 12 : 18;
                paint.setTextSize(size * unit * control.scale);
                canvas.drawText(control.label, x, y - (paint.ascent() + paint.descent()) * 0.5f, paint);
            }
        }
    }
}

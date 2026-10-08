#version 460
layout(push_constant) uniform Input { vec4 value; } input_value;
layout(location=0) out vec4 interpolator;
void main() {
  vec2 positions[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
  gl_Position=vec4(positions[gl_VertexIndex],0,1);
  interpolator=input_value.value;
}

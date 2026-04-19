#version 460 core

layout (location=0) in vec3 pos;
out vec2 uv;

uniform vec2 destPos;
uniform vec2 destSize;

void main () {
  gl_Position = vec4(pos.xy * destSize + destPos, 0.0, 1.0);
  gl_Position.y *= -1.0;
  uv = pos.xy;
}
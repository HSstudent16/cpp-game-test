#version 460 core

in vec2 uv;
out vec4 color;

uniform vec2 sourcePos;
uniform vec2 sourceSize;

uniform sampler2D tex;

void main () {
  vec2 mapped_uv = uv * sourceSize + sourcePos;
  color = texture(tex, mapped_uv);
  if (color.a <= 0.0) {
    discard;
  }
}
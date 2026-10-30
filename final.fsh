#version 120
uniform sampler2D colortex0;
varying vec2 texcoord;
void main() {
  vec3 c = texture2D(colortex0, texcoord).rgb;
  c = c / (c + vec3(1.0)) * 1.5;
  gl_FragColor = vec4(c, 1.0);
}

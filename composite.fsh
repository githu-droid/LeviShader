#version 120
#include "/lib/settings.glsl"
uniform sampler2D colortex0;
uniform sampler2D depthtex0;
uniform float viewWidth;
uniform float viewHeight;
varying vec2 texcoord;
/* RENDERTARGETS: 0,1 */
void main() {
  vec3 c = texture2D(colortex0, texcoord).rgb;
  float d = texture2D(depthtex0, texcoord).r;
  vec3 b = texture2D(colortex0, texcoord + vec2(1.0/viewWidth, 0.0)).rgb * BLOOM_STRENGTH;
  gl_FragData[0] = vec4(c + b * 0.1, 1.0);
  gl_FragData[1] = vec4(b, d);
}

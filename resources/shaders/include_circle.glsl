#ifndef INCLUDE_CIRCLE_GLSL
#define INCLUDE_CIRCLE_GLSL

vec4 getCircle(in vec2 t){

	float dist = length(t - vec2(0.5)); // distance will be at max 0.5
	dist *= 2; // scale dist to 0..1 range
	float circle_smoothness = 0.2;
	float circle_radius = 1;
	float circle = 1. - smoothstep( circle_radius - circle_smoothness, circle_radius, dist);

	return vec4(vec3(1), circle);
}

#endif
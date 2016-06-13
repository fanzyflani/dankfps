#version 150

in vec4 g2f_light0_pos;
in vec4 g2f_vertex;
in vec4 g2f_world_vertex;
in vec4 g2f_normal;
in vec4 g2f_world_normal;

void main()
{
	const float amb = 0.1;

	vec3 normal = normalize(g2f_normal.xyz);
	vec3 camdir = normalize(g2f_vertex.xyz - g2f_light0_pos.xyz);

	vec3 wv = g2f_world_vertex.xyz;
	float gridx = dot(wv,g2f_world_normal.yzx*vec3( 1.0f, 1.0f, 1.0f));
	float gridy = dot(wv,g2f_world_normal.zxy*vec3( 1.0f, 1.0f, 1.0f));
	gridx -= floor(gridx);
	gridy -= floor(gridy);

	float doof = (gridx < 0.1 || gridy < 0.1 ? 0.5 : 1.0);
	//doof = doof - floor(doof);
	doof = (0.5+0.5*doof);

	float diff = max(0.0, -dot(normal, camdir));
	diff = diff*(1.0-amb)+amb;
	gl_FragData[0] = vec4(vec3(1.0, 0.7, 0.5)*doof*diff, 1.0);
}

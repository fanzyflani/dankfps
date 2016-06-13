#version 150

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

uniform mat4 Mproj, Mcam;

in vec4 v2g_light0_pos[];
in vec4 v2g_vertex[];
in vec4 v2g_world_vertex[];
out vec4 g2f_light0_pos;
out vec4 g2f_vertex;
out vec4 g2f_world_vertex;
out vec4 g2f_world_normal;
out vec4 g2f_normal;

void main()
{
	g2f_light0_pos = Mcam * vec4(vec3(0.0, 2.5, -5.0), 1.0);

	vec3 p0 = v2g_world_vertex[0].xyz;
	vec3 p1 = v2g_world_vertex[1].xyz;
	vec3 p2 = v2g_world_vertex[2].xyz;
	g2f_world_normal = vec4(normalize(cross(p1-p0, p2-p0)), 0.0);
	g2f_normal = Mcam * g2f_world_normal;

	for(int i = 0; i < 3; i++) {
		gl_Position = gl_in[i].gl_Position;
		g2f_world_vertex = v2g_world_vertex[i];
		g2f_vertex = v2g_vertex[i];
		EmitVertex();
	}

}


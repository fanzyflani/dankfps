#version 150

uniform vec4 Voffs;
uniform float Aroty;
uniform mat4 Mproj, Mcam;
in vec4 in_vertex;

out vec4 v2g_vertex;
out vec4 v2g_world_vertex;

void main()
{
	v2g_world_vertex = (0.0
		+ in_vertex.xyzw * vec4( 0.0f, 1.0f, 0.0f, 1.0f)
		+ cos(Aroty)*(in_vertex.xyzw * vec4( 1.0f, 0.0f, 1.0f, 0.0f))
		+ sin(Aroty)*(in_vertex.zyxw * vec4(-1.0f, 0.0f, 1.0f, 0.0f)));
	v2g_vertex = Mcam * (v2g_world_vertex + Voffs);
	gl_Position = Mproj * v2g_vertex;
}


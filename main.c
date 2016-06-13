#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#ifndef SERVER
#include <epoxy/gl.h>
#include <SDL.h>
#endif
#include <math.h>

#include <unistd.h>
#include <sys/time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "linmath.h"

#define P_IN_MOVE_LEFT 0x0001
#define P_IN_MOVE_RIGHT 0x0002
#define P_IN_MOVE_FORWARD 0x0004
#define P_IN_MOVE_BACK 0x0008
#define P_IN_JUMP 0x0010

#define P_FL_GROUNDED 0x0001
#define P_FL_INGAME 0x0002

// TODO: delta compression (we are NOT sending 1056 bytes per world update!)
#define PLAYER_MAX 8

typedef int32_t fixed32;

struct player {
	uint16_t ry, rx;
	uint16_t input;
	uint16_t flags;
	fixed32 px, py, pz;
	fixed32 vx, vy, vz;
} __attribute__((__packed__));

struct keyframe {
	uint16_t timestamp;
	struct player players[PLAYER_MAX];
} __attribute__((__packed__));

struct keyframe keyframes[2];
uint32_t next_phystick = 0;
uint16_t state_timestamp = 0;
int net_sockfd = -1;

const uint8_t pkt_init[] = "\x21\x21\x35";
const uint8_t pkt_kill[] = "\x21\x21\x3E";

#ifdef SERVER

struct sockaddr *connected_addr[PLAYER_MAX];
socklen_t connected_addrlen[PLAYER_MAX];

#else
socklen_t target_addrlen;
struct sockaddr *target_addr;
int client_player_idx = -1;
uint32_t last_phystick = 0;
uint32_t last_intick = 0;
uint32_t next_intick = 0;
SDL_Window *window;
SDL_GLContext context;

#define INIT_WIDTH 1280
#define INIT_HEIGHT 720

GLfloat mesh_room[][6] = {
	// -Y (floor)
	{-10.0f, -3.0f, -10.0f, },
	{-10.0f, -3.0f,  10.0f, },
	{ 10.0f, -3.0f, -10.0f, },
	{ 10.0f, -3.0f,  10.0f, },
	{ 10.0f, -3.0f, -10.0f, },
	{-10.0f, -3.0f,  10.0f, },

	// -X
	{-10.0f,  3.0f, -10.0f, },
	{-10.0f,  3.0f,  10.0f, },
	{-10.0f, -3.0f, -10.0f, },
	{-10.0f, -3.0f,  10.0f, },
	{-10.0f, -3.0f, -10.0f, },
	{-10.0f,  3.0f,  10.0f, },

	// +X
	{ 10.0f,  3.0f, -10.0f, },
	{ 10.0f, -3.0f, -10.0f, },
	{ 10.0f,  3.0f,  10.0f, },
	{ 10.0f, -3.0f,  10.0f, },
	{ 10.0f,  3.0f,  10.0f, },
	{ 10.0f, -3.0f, -10.0f, },

	// -Z
	{-10.0f,  3.0f, -10.0f, },
	{-10.0f, -3.0f, -10.0f, },
	{ 10.0f,  3.0f, -10.0f, },
	{ 10.0f, -3.0f, -10.0f, },
	{ 10.0f,  3.0f, -10.0f, },
	{-10.0f, -3.0f, -10.0f, },

	// +Z
	{-10.0f,  3.0f,  10.0f, },
	{ 10.0f,  3.0f,  10.0f, },
	{-10.0f, -3.0f,  10.0f, },
	{ 10.0f, -3.0f,  10.0f, },
	{-10.0f, -3.0f,  10.0f, },
	{ 10.0f,  3.0f,  10.0f, },
};
GLuint mesh_room_vao;
GLuint mesh_room_vbo;

GLfloat mesh_player[][3] = {
	{ 0.0f, 2.0f,  0.0f, },
	{ 0.0f, 0.0f, -1.5f, },
	{-1.0f, 0.0f,  0.0f, },

	{ 0.0f, 2.0f,  0.0f, },
	{-1.0f, 0.0f,  0.0f, },
	{ 1.0f, 0.0f,  0.0f, },

	{ 0.0f, 2.0f,  0.0f, },
	{ 1.0f, 0.0f,  0.0f, },
	{ 0.0f, 0.0f, -1.5f, },

	{-1.0f, 0.0f,  0.0f, },
	{ 0.0f, 0.0f, -1.5f, },
	{ 1.0f, 0.0f,  0.0f, },
};
GLuint mesh_player_vao;
GLuint mesh_player_vbo;

#endif

uint32_t time_in_usecs(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	uint32_t s = tv.tv_sec;
	uint32_t u = tv.tv_usec;

	return (s * 1000000U) + u;
}

void *load_file_to_string(const char *fname)
{
	FILE *fp = fopen(fname, "rb");
	size_t buf_size = 0;
	char *buf = NULL;
	
	for(;;)
	{
		char subbuf[1024];
		ssize_t blen = fread(subbuf, 1, 1024, fp);
		if(blen < 0) {
			perror("load_file_to_string:fread");
			abort();
		}
		if(blen == 0) {
			break;
		}
		buf_size += blen;
		buf = realloc(buf, buf_size+1);
		if(buf == NULL) {
			perror("load_file_to_string:realloc");
			abort();
		}
		memcpy(buf+buf_size-blen, subbuf, blen);
	}

	fclose(fp);

	if(buf == NULL) {
		buf = malloc(1);
	}

	buf[buf_size] = '\x00';

	return buf;
}

#ifndef SERVER
void show_shader_log(GLuint shader, const char *desc)
{
	GLint loglen = 4096;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &loglen);
	if(loglen < 1) { loglen = 1; }
	loglen++;
	char *buf = malloc(loglen+1);
	GLsizei real_loglen = 0;
	glGetShaderInfoLog(shader, loglen, &real_loglen, buf);
	printf("=== %s (%d/%d) === {\n%s\n}\n", desc, real_loglen, loglen, buf);
	free(buf);
}

void show_program_log(GLuint program, const char *desc)
{
	GLint loglen = 4096;
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &loglen);
	if(loglen < 1) { loglen = 1; }
	loglen++;
	char *buf = malloc(loglen+1);
	GLsizei real_loglen = 0;
	glGetProgramInfoLog(program, loglen, &real_loglen, buf);
	printf("=== %s (%d/%d) === {\n%s\n}\n", desc, real_loglen, loglen, buf);
	free(buf);
}

void add_shader(GLuint program, const char *fname, GLenum category)
{
	const char *catheader = NULL;

	switch(category)
	{
		case GL_COMPUTE_SHADER: catheader = "COMPUTE SHADER"; break;
		case GL_VERTEX_SHADER: catheader = "VERTEX SHADER"; break;
		case GL_TESS_CONTROL_SHADER: catheader = "TESSELATION CONTROL SHADER"; break;
		case GL_TESS_EVALUATION_SHADER: catheader = "TESSELATION EVALUATION SHADER"; break;
		case GL_GEOMETRY_SHADER: catheader = "GEOMETRY SHADER"; break;
		case GL_FRAGMENT_SHADER: catheader = "FRAGMENT SHADER"; break;
		default: abort(); for(;;) {} return;
	}

	GLchar *src_str = load_file_to_string(fname);
	GLchar const* src_str_cast = src_str;
	GLuint shader_obj = glCreateShader(category);
	glShaderSource(shader_obj, 1, &src_str_cast, NULL);
	glCompileShader(shader_obj);
	show_shader_log(shader_obj, catheader);
	glAttachShader(program, shader_obj);
}
#endif

void phys_update_player(struct player *p, float time_delta)
{
	// bail out ASAP if slot free
	if(!(p->flags & P_FL_INGAME)) { return; }

	// adjustables
	const float JUMP_SPEED = 10.0f;
	const float JUMP_BPM = 120.0f;
	const float SPEED_CAP = 4.0f;
	const float mvspeed_x = 20.0f;
	const float mvspeed_z = 20.0f;

	// calculate angles
	float ry = p->ry*M_PI*2.0/65536.0;
	//float rx = (p->rx-0x8000)*M_PI*2.0/65536.0;
	float rsy = sin(ry);
	float rcy = cos(ry);

	// calculate local motion
	int mvxi = 0;
	int mvzi = 0;
	if(p->input & P_IN_MOVE_LEFT) { mvxi--; }
	if(p->input & P_IN_MOVE_RIGHT) { mvxi++; }
	if(p->input & P_IN_MOVE_FORWARD) { mvzi--; }
	if(p->input & P_IN_MOVE_BACK) { mvzi++; }
	float mvx = ((float)mvxi) * mvspeed_x;
	float mvz = ((float)mvzi) * mvspeed_z;

	// local to global motion
	float world_mvx = mvx*rcy - mvz*rsy;
	float world_mvz = mvz*rcy + mvx*rsy;

	// gravity acceleration
	float accel_grav = -(JUMP_SPEED*JUMP_BPM*2.0f/60.0f) * time_delta;

	// fetch current ("old") position + velocity
	float ox = p->px / 65536.0f;
	float oy = p->py / 65536.0f;
	float oz = p->pz / 65536.0f;
	float ovx = p->vx / 65536.0f;
	float ovy = p->vy / 65536.0f;
	float ovz = p->vz / 65536.0f;

	// calculate new horizontal velocity
	float nvx = ovx + world_mvx * time_delta;
	float nvz = ovz + world_mvz * time_delta;

	// calculate normalised motion direction
	//
	// XXX: this could be optimised to not use a sqrt
	// (well OK, we WOULD use sqrt(2.0), but as a constant)
	float world_mvd = sqrtf(world_mvx*world_mvx + world_mvz*world_mvz);
	float world_dvx = world_mvx/world_mvd;
	float world_dvz = world_mvz/world_mvd;

	// calculate speed relative to motion detection
	float dot_omove = world_dvx*ovx + world_dvz*ovz;
	float dot_nmove = world_dvx*nvx + world_dvz*nvz;

	// apply speed cap
	if(dot_nmove < SPEED_CAP) {
		p->vx = (nvx) * 65536.0f;
		p->vz = (nvz) * 65536.0f;
	} else if(dot_nmove > dot_omove && dot_omove < SPEED_CAP) {
		float cap_time = (SPEED_CAP - dot_omove) / (dot_nmove - dot_omove);
		p->vx = (ovx + (nvx-ovx)*cap_time) * 65536.0f;
		p->vz = (ovz + (nvz-ovz)*cap_time) * 65536.0f;
	}

	// calculate new position
	float dx = ovx*time_delta;
	float dy = (ovy + accel_grav/2.0f)*time_delta;
	float dz = ovz*time_delta;

	float hit_time = 1.0f;
	float nx = ox + dx*hit_time;
	float ny = oy + dy*hit_time;
	float nz = oz + dz*hit_time;
	p->vy = (ovy + accel_grav)*65536.0f;

	// set grounding
	if(ny < -3.0f) {
		ny = -3.0f;
		p->vy = 0.0f * 65536.0f;

		p->flags |= P_FL_GROUNDED;
	}

	// apply friction
	if(p->flags & P_FL_GROUNDED) {
		float frictmul = exp(-time_delta*5.0f);
		p->vx *= frictmul;
		p->vz *= frictmul;
	}

	// handle jump
	if((p->input & P_IN_JUMP) && (p->flags & P_FL_GROUNDED)) {
		p->vy = JUMP_SPEED * 65536.0f;
		p->flags &= ~P_FL_GROUNDED;
	}

	// write new position
	p->px = nx * 65536.0f;
	p->py = ny * 65536.0f;
	p->pz = nz * 65536.0f;
}

// p0 = newest
// p1 = oldest
void phys_lerp_player(struct player *p, const struct player *p0, const struct player *p1, float time)
{
	// if we aren't in yet, drop out
	if(time <= 0.0f) {
		memcpy(p, p1, sizeof(struct player));
		return;
	}

	memcpy(p, p0, sizeof(struct player));

	if(time >= 1.0f) {
		return;
	}

	p->px = (p1->px) + (p0->px - p1->px)*time;
	p->py = (p1->py) + (p0->py - p1->py)*time;
	p->pz = (p1->pz) + (p0->pz - p1->pz)*time;
	p->rx = (p1->rx) + (p0->rx - p1->rx)*time;
	int32_t ry = (int16_t)(p1->ry);
	int32_t dry = (int16_t)(p0->ry - p1->ry);
	p->ry = (uint16_t)(ry + dry*time);
}

void phys_update(float delta_time)
{
	int i;

	// save old keyframe
	memcpy(&keyframes[1], &keyframes[0], sizeof(struct keyframe));

	// do players
	for(i = 0; i < PLAYER_MAX; i++) {
		phys_update_player(&keyframes[0].players[i], delta_time);
	}

	// update counter
	state_timestamp += 1;
	keyframes[0].timestamp = state_timestamp;
}

#ifndef SERVER
void client_loop(void)
{
	int i;

	mat4x4 Mproj, Mcam;
	mat4x4_perspective(Mproj, 90.0f*M_PI/180.0f, ((float)INIT_WIDTH)/INIT_HEIGHT, 0.04f, 500.0f);

	mat4x4_identity(Mcam);
	mat4x4_translate_in_place(Mcam, 0.0f, 0.0f, -11.0f);

	GLuint prog_base = glCreateProgram();
	add_shader(prog_base, "base.vert.glsl", GL_VERTEX_SHADER);
	add_shader(prog_base, "base.geom.glsl", GL_GEOMETRY_SHADER);
	add_shader(prog_base, "base.frag.glsl", GL_FRAGMENT_SHADER);
	glBindAttribLocation(prog_base, 0, "in_vertex");
	glLinkProgram(prog_base);
	show_program_log(prog_base, "FINAL PROGRAM LINK");
	glUseProgram(prog_base);

	glGenVertexArrays(1, &mesh_room_vao);
	glBindVertexArray(mesh_room_vao);
	glGenBuffers(1, &mesh_room_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, mesh_room_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(mesh_room), mesh_room, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(mesh_room[0]), &((GLfloat *)0)[0]);
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	glGenVertexArrays(1, &mesh_player_vao);
	glBindVertexArray(mesh_player_vao);
	glGenBuffers(1, &mesh_player_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, mesh_player_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(mesh_player), mesh_player, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(mesh_player[0]), &((GLfloat *)0)[0]);
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);

	SDL_CaptureMouse(SDL_TRUE);
	SDL_SetRelativeMouseMode(SDL_TRUE);

	last_phystick = next_phystick = time_in_usecs();
	last_intick = next_intick = time_in_usecs();

	struct player *pself0 = &keyframes[0].players[client_player_idx];
	pself0->px = 4*0x10000;
	pself0->py = -3*0x10000;
	pself0->pz = 5*0x10000;
	pself0->vx = 0*0x10000;
	pself0->vy = 0*0x10000;
	pself0->vz = 0*0x10000;
	pself0->ry = 0x0000;
	pself0->rx = 0x8000;
	pself0->input = 0;
	pself0->flags = P_FL_INGAME;
	keyframes[0].players[1].flags = P_FL_INGAME;
	keyframes[0].players[1].input = P_IN_JUMP;
	for(;;)
	{
		SDL_Event ev;
		while(SDL_PollEvent(&ev)) {
		switch(ev.type) {
			case SDL_KEYUP: switch(ev.key.keysym.sym) {
				case SDLK_w: pself0->input &= ~P_IN_MOVE_FORWARD; break;
				case SDLK_s: pself0->input &= ~P_IN_MOVE_BACK; break;
				case SDLK_a: pself0->input &= ~P_IN_MOVE_LEFT; break;
				case SDLK_d: pself0->input &= ~P_IN_MOVE_RIGHT; break;
				case SDLK_SPACE: pself0->input &= ~P_IN_JUMP; break;

				case SDLK_ESCAPE: return;
			} break;
			case SDL_KEYDOWN: switch(ev.key.keysym.sym) {
				case SDLK_w: pself0->input |= P_IN_MOVE_FORWARD; break;
				case SDLK_s: pself0->input |= P_IN_MOVE_BACK; break;
				case SDLK_a: pself0->input |= P_IN_MOVE_LEFT; break;
				case SDLK_d: pself0->input |= P_IN_MOVE_RIGHT; break;
				case SDLK_SPACE: pself0->input |= P_IN_JUMP; break;
			} break;

			case SDL_MOUSEMOTION:
				pself0->ry += ev.motion.xrel*65536/1000;
				pself0->rx -= ev.motion.yrel*65536/1000;
				if(pself0->rx < 0x8000-0x4000) { 
					pself0->rx = 0x8000-0x4000;
				}
				if(pself0->rx > 0x8000+0x4000) { 
					pself0->rx = 0x8000+0x4000;
				}
				break;

			case SDL_QUIT:
				return;
		}
		}

		// Get messages
		uint32_t cur_time = time_in_usecs();

		int new_phystick = 0;

		for(;;) {
			uint8_t msg_buf[1024];
			size_t msg_buf_len = sizeof(msg_buf);
			uint8_t msg_sa_buf[128];
			struct sockaddr *msg_sa = (struct sockaddr *)msg_sa_buf;
			socklen_t msg_sa_len = sizeof(msg_sa_buf);
			ssize_t msg_len = recvfrom(net_sockfd, msg_buf, msg_buf_len,
				MSG_DONTWAIT, msg_sa, &msg_sa_len);

			// Handle errors
			if(msg_len < 0) {
				int e = errno;
				if(e == EAGAIN || e == EWOULDBLOCK) {
					break;
				} else {
					// TODO: handle gracefully
					perror("recvfrom");
					return;
				}
			}

			//printf("MSG %d bytes\n", (int)msg_len);

			if(msg_len < 3) {
				continue;
			}

			if(msg_buf[2] == 0x3E) {
				printf("Connection terminated\n");
				return;
			}

			if(msg_buf[2] == 0x20 && msg_len == 3+3+sizeof(struct keyframe)) {
				//printf("World update packet\n");
				if(!new_phystick) {
					memcpy(&keyframes[1], &keyframes[0],
						sizeof(struct keyframe));
					new_phystick = 1;
					last_phystick = cur_time;
					next_phystick = time_in_usecs() + 1000000/20;
				}

				uint16_t rx = keyframes[0].players[client_player_idx].rx;
				uint16_t ry = keyframes[0].players[client_player_idx].ry;
				uint16_t input = keyframes[0].players[client_player_idx].input;

				// TODO: reassign player if necessary

				memcpy(&keyframes[0], msg_buf+6, sizeof(struct keyframe));
				keyframes[0].players[client_player_idx].rx = rx;
				keyframes[0].players[client_player_idx].ry = ry;
				keyframes[0].players[client_player_idx].input = input;
			}
		}

		/*
		int32_t physdelta = (int32_t)(cur_time - next_phystick);

		if(physdelta > 0) {
			while(physdelta > 0) {
				//printf("tick\n");
				phys_update(1.0f / 20.0f);
				next_phystick += 1000000/20;
				physdelta -= 1000000/20;
			}

			last_phystick = cur_time;
		}
		*/

		int32_t indelta = (int32_t)(cur_time - next_intick);

		if(indelta > 0) {
			uint8_t msg_buf[2048];
			size_t msg_buf_max_len = sizeof(msg_buf);
			size_t msg_buf_len = 0;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = 0x21;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = 0x23;
			memcpy(msg_buf+msg_buf_len,
				&keyframes[0].players[client_player_idx],
				8);
			msg_buf_len += 8;
			assert(msg_buf_len < msg_buf_max_len);

			sendto(net_sockfd, msg_buf, msg_buf_len,
				0, target_addr, target_addrlen);

			while(indelta > 0) {
				next_intick += 1000000/40;
				indelta -= 1000000/40;
			}

			last_intick = cur_time;
		}

		uint32_t ureltick = cur_time - last_phystick;
		uint32_t udeltick = next_phystick - last_phystick;
		int32_t reltick = (int32_t)(ureltick);
		int32_t deltick = (int32_t)(udeltick);
		float lerp_time = ((float)reltick)/deltick;

		mat4x4 MA, MB;
		struct player bp;
		phys_lerp_player(
			&bp,
			&keyframes[0].players[client_player_idx],
			&keyframes[1].players[client_player_idx],
			lerp_time);

		mat4x4_identity(MA);
		mat4x4_rotate_X(MB, MA, -(bp.rx+0x8000)*M_PI*2.0f/65536.0f);
		mat4x4_rotate_Y(Mcam, MB, -bp.ry*M_PI*2.0f/65536.0f);
		mat4x4_translate_in_place(Mcam, -bp.px/65536.0f, -(bp.py/65536.0f+1.5f), -bp.pz/65536.0f);

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUniformMatrix4fv(glGetUniformLocation(prog_base, "Mproj"), 1, GL_FALSE, Mproj[0]);
		glUniformMatrix4fv(glGetUniformLocation(prog_base, "Mcam"), 1, GL_FALSE, Mcam[0]);

		glUniform1f(glGetUniformLocation(prog_base, "Aroty"), 0.0f);
		glUniform4f(glGetUniformLocation(prog_base, "Voffs"), 0.0f, 0.0f, 0.0f, 0.0f);
		glBindVertexArray(mesh_room_vao);
		glDrawArrays(GL_TRIANGLES, 0, sizeof(mesh_room)/sizeof(mesh_room[0]));

		for(i = 0; i < PLAYER_MAX; i++) {
			if(i == client_player_idx) {
				continue;
			}

			if(!(keyframes[0].players[i].flags & P_FL_INGAME)) {
				continue;
			}

			phys_lerp_player(
				&bp,
				&keyframes[0].players[i],
				&keyframes[1].players[i],
				lerp_time);

			int px = ((bp.px));
			int py = ((bp.py));
			int pz = ((bp.pz));
			int ry = ((int)(bp.ry));
			//int rx = ((int)(bp.rx));

			glUniform1f(glGetUniformLocation(prog_base, "Aroty")
				, ry*M_PI*2.0/65536.0);
			glUniform4f(glGetUniformLocation(prog_base, "Voffs")
				, px/65536.0f, py/65536.0f, pz/65536.0f, 0.0f);
			glBindVertexArray(mesh_player_vao);
			glDrawArrays(GL_TRIANGLES, 0, sizeof(mesh_player)/sizeof(mesh_player[0]));
		}

		glBindVertexArray(0);

		SDL_GL_SwapWindow(window);
		usleep(100);
	}
}
#endif

#ifdef SERVER
void server_loop(void)
{
	int i;

	printf("Server loop started\n");
	next_phystick = time_in_usecs();
	for(;;) {
		// Simulate physics
		uint32_t cur_time = time_in_usecs();
		int32_t physdelta = (int32_t)(cur_time - next_phystick);
		if(physdelta > 0) {
			while(physdelta > 0) {
				phys_update(1.0f/20.0f);
				next_phystick += 1000000/20;
				physdelta -= 1000000/20;
			}
		}

		// Receive messages
		for(;;) {
			uint8_t msg_sa_buf[128];
			uint8_t msg_buf[2048];
			size_t msg_buf_len = sizeof(msg_buf);
			struct sockaddr *msg_sa = (struct sockaddr *)msg_sa_buf;
			socklen_t msg_sa_len = sizeof(msg_sa_buf);

			// Get message
			ssize_t msg_len = recvfrom(net_sockfd, msg_buf, msg_buf_len,
				MSG_DONTWAIT, msg_sa, &msg_sa_len);
			//printf("msg=%4d sa=%4d\n", msg_len, msg_sa_len);

			// Handle errors
			if(msg_len < 0) {
				int e = errno;
				if(e == EAGAIN || e == EWOULDBLOCK) {
					break;
				} else {
					// TODO: handle gracefully
					perror("recvfrom");
					abort();
				}
			}

			// Drop if address too large
			if(msg_sa_len > sizeof(msg_sa_buf)) {
				continue;
			}

			// Drop if packet too small
			if(msg_len < 3) {
				continue;
			}

			// Determine client
			int msg_client = -1;
			for(i = 0; i < PLAYER_MAX; i++) {
				if(connected_addr[i] == NULL) { continue; }
				if(connected_addrlen[i] != msg_sa_len) { continue; }
				if(memcmp(connected_addr[i], msg_sa, msg_sa_len)) { continue; }
				msg_client = i;
				break;
			}

			// Determine which path we need to take
			/*
			uint16_t pkt_last_state = (0
				|(((uint16_t)msg_buf[0])<<0)
				|(((uint16_t)msg_buf[1])<<8)
				);
			*/
			uint8_t pkt_ptyp = msg_buf[2];

			if(msg_client < 0) {
				if(pkt_ptyp == 0x35) {
					// Allocate client
					for(i = 0; i < PLAYER_MAX; i++) {
						if(connected_addr[i] != NULL) { continue; }
						printf("Player %d connected\n", i);
						connected_addr[i] = malloc(msg_sa_len);
						memcpy(connected_addr[i], msg_sa, msg_sa_len);
						connected_addrlen[i] = msg_sa_len;
						keyframes[0].players[i].rx = 0x8000;
						keyframes[0].players[i].flags = P_FL_INGAME;
						msg_client = i;
						sendto(net_sockfd, pkt_init, sizeof(pkt_init),
							0, msg_sa, msg_sa_len);
						break;
					}

					if(msg_client < 0) {
						printf("POOL'S CLOSED\n");
						sendto(net_sockfd, pkt_kill, sizeof(pkt_kill),
							0, msg_sa, msg_sa_len);
					}

				} else {
					sendto(net_sockfd, pkt_kill, sizeof(pkt_kill),
						0, msg_sa, msg_sa_len);
				}
			} else {
				if(pkt_ptyp == 0x3E) {
					// Deallocate client
					free(connected_addr[msg_client]);
					connected_addrlen[msg_client] = 0;
					memset(&keyframes[0].players[msg_client], 0,
						sizeof(struct player));

				} else if(pkt_ptyp == 0x21) {
					// Ensure correct packet length
					if(msg_len != 3+2+8) {
						// Deallocate client
						free(connected_addr[msg_client]);
						connected_addrlen[msg_client] = 0;
						memset(&keyframes[0].players[msg_client], 0,
							sizeof(struct player));
					} else {
						// Update keyframe
						/*
						uint16_t pkt_intime = (0
							|(((uint16_t)msg_buf[3])<<0)
							|(((uint16_t)msg_buf[4])<<8)
							);
						*/
						struct player *sp = (struct player *)(msg_buf+5);
						struct player *dp = &keyframes[0].players[msg_client];
						dp->input = sp->input;
						dp->ry = sp->ry;
						dp->rx = sp->rx;
						/*
						memcpy(&keyframes[0].players[msg_client],
							msg_buf+5,
							sizeof(struct player));
						*/
					}
				}
				continue;
			}
		}

		// Send messages
		//
		// Why do we do a new world update per player?
		// Simple - we will need to use delta compression at some stage.
		// 
		for(i = 0; i < PLAYER_MAX; i++) {
			if(connected_addr[i] == NULL) {
				continue;
			}

			uint8_t msg_buf[2048];
			size_t msg_buf_max_len = sizeof(msg_buf);
			size_t msg_buf_len = 0;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = 0x20;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = 0x23;
			msg_buf[msg_buf_len++] = i;
			memcpy(msg_buf+msg_buf_len, &keyframes[0], sizeof(keyframes[0]));
			msg_buf_len += sizeof(keyframes[0]);
			assert(msg_buf_len < msg_buf_max_len);

			sendto(net_sockfd, msg_buf, msg_buf_len,
				0, connected_addr[i], connected_addrlen[i]);
		}

		// Sleep
		usleep(-physdelta);
	}
}

#endif

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	memset(&keyframes, 0, sizeof(keyframes));

#ifdef SERVER
	// Server init + loop

	assert(argc > 1);
	const char *portstr = argv[1];
	int port = atoi(portstr);
	printf("Binding to port %d...\n", port);
	struct addrinfo hadi;
	struct addrinfo *adi;
	memset(&hadi, 0, sizeof(hadi));
	hadi.ai_family = AF_UNSPEC;
	hadi.ai_socktype = SOCK_DGRAM;
	hadi.ai_protocol = 0;
	hadi.ai_flags = AI_PASSIVE;
	int gai_result = getaddrinfo(NULL, portstr, &hadi, &adi);
	if(gai_result != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_result));
		return 1;
	}

	int yes=1;
	net_sockfd = socket(adi->ai_family, adi->ai_socktype, adi->ai_protocol);
	if(net_sockfd <= 0) {
		perror("socket");
		freeaddrinfo(adi);
		return 1;
	}

	if(setsockopt(net_sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
		perror("setsockopt");
		freeaddrinfo(adi);
		return 1;
	}

	if(bind(net_sockfd, adi->ai_addr, adi->ai_addrlen) < 0) {
		perror("bind");
		freeaddrinfo(adi);
		return 1;
	}

	freeaddrinfo(adi);

	printf("Now listening!\n");
	server_loop();
	printf("Cleaning up...\n");
	close(net_sockfd);
	printf("Done!\n");

#else
	// Client init + loop
	SDL_assert_release(argc > 2);
	const char *hostname = argv[1];
	const char *portstr = argv[2];
	printf("Resolving host %s, UDP port %s...\n", hostname, portstr);
	struct addrinfo hadi;
	struct addrinfo *adi;
	memset(&hadi, 0, sizeof(hadi));
	hadi.ai_family = AF_UNSPEC;
	hadi.ai_socktype = SOCK_DGRAM;
	hadi.ai_protocol = 0;
	hadi.ai_flags = 0;
	int gai_result = getaddrinfo(hostname, portstr, &hadi, &adi);
	if(gai_result != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_result));
		return 1;
	}

	net_sockfd = socket(adi->ai_family, adi->ai_socktype, adi->ai_protocol);
	if(net_sockfd <= 0) {
		perror("socket");
		freeaddrinfo(adi);
		return 1;
	}


	char addr_name_buf[256];
	addr_name_buf[0] = '\x00';
	addr_name_buf[sizeof(addr_name_buf)-1] = '\x00';
	inet_ntop(adi->ai_family, (adi->ai_family == AF_INET6
			? (void *)&((struct sockaddr_in6 *)(adi->ai_addr))->sin6_addr
			: (void *)&((struct sockaddr_in *)(adi->ai_addr))->sin_addr
			)
		, addr_name_buf, sizeof(addr_name_buf)-1);
	printf("Connected to %s\n", addr_name_buf);
	//printf("%d\n", ntohs(((struct sockaddr_in *)(adi->ai_addr))->sin_port));
	target_addrlen = adi->ai_addrlen;
	target_addr = malloc(target_addrlen);
	memcpy(target_addr, adi->ai_addr, target_addrlen);
	freeaddrinfo(adi);

	printf("Establishing connection...\n");

	int attempts_remain = 20;

	while(client_player_idx < 0)
	{
		if(attempts_remain <= 0) {
			printf("Cannot connect\n");
			return 1;
		}

		printf("Attempts remaining: %d\n", attempts_remain);
		attempts_remain--;
		sendto(net_sockfd, pkt_init, sizeof(pkt_init),
			0, target_addr, target_addrlen);
		usleep(200000);

		// Get message
		for(;;) {
			uint8_t msg_buf[1024];
			size_t msg_buf_len = sizeof(msg_buf);
			uint8_t msg_sa_buf[128];
			struct sockaddr *msg_sa = (struct sockaddr *)msg_sa_buf;
			socklen_t msg_sa_len = sizeof(msg_sa_buf);
			ssize_t msg_len = recvfrom(net_sockfd, msg_buf, msg_buf_len,
				MSG_DONTWAIT, msg_sa, &msg_sa_len);

			// Handle errors
			if(msg_len < 0) {
				int e = errno;
				if(e == EAGAIN || e == EWOULDBLOCK) {
					break;
				} else {
					// TODO: handle gracefully
					perror("recvfrom");
					return 1;
				}
			}

			printf("MSG %d bytes\n", (int)msg_len);

			if(msg_len < 3) {
				continue;
			}

			if(msg_buf[2] == 0x3E) {
				printf("Connection terminated immediately\n");
				return 1;
			}

			if(msg_buf[2] == 0x20 && msg_len == 3+3+sizeof(struct keyframe)) {
				client_player_idx = msg_buf[5];
				memcpy(&keyframes[1], msg_buf+6, sizeof(struct keyframe));
				memcpy(&keyframes[0], msg_buf+6, sizeof(struct keyframe));
				printf("Connected as player %d\n", client_player_idx);
				SDL_assert_release(client_player_idx >= 0
					&& client_player_idx < PLAYER_MAX);
				break;
			}
		}
	}

	SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE);

	window = SDL_CreateWindow("dankfps",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		INIT_WIDTH, INIT_HEIGHT,
		SDL_WINDOW_OPENGL);
	SDL_assert_release(window != NULL);

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

	context = SDL_GL_CreateContext(window);
	SDL_assert_release(context);

	//SDL_GL_SetSwapInterval(0); // disable vsync
	SDL_GL_SetSwapInterval(-1); // late swap tearing if you want it

	client_loop();
#endif

	return 0;
}


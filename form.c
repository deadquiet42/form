#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

#define GRID 7
#define FACES 6
#define VERTICES (FACES * GRID * GRID)
#define WIDTH 640
#define HEIGHT 480
#define SAMPLE_RATE 48000
#define PI 3.14159265358979323846f
#define VIEW_SIZE 1.8f
#define OUTPUT_GAIN 1.33f
#define HIT_GAIN 0.20f
#define PULL_LIFT -0.08f
#define DENT_SQUEEZED 0.4f
#define EXAGGERATE 1.5f
#define RELEASE 0.6f
#define INNER_POINTS 216
#define VOICE_GAIN 1.7f
#define WIDTH_DELAY 480
#define VOICE_WIDTH 0.32f

typedef struct { float x, y, z; } Vec3;
typedef struct { Vec3 rest, position, velocity; } Vertex;
typedef struct { float x, y, depth; Vec3 world; } ScreenVertex;

typedef struct {
    float lx, ly, slx, sly, rx, ry;
    float ltrigger, rtrigger;
    int compress, expand, disturb, colour;
    int damping, resonance, pull, push;
    int rot_up, rot_down, rot_left, rot_right;
    int knock, tick;
} Controls;

enum { HIT_STRIKE, HIT_KNOCK, HIT_TICK };
#define HITS 16
#define HIT_QUEUE 32
#define LOOKAHEAD 48
typedef struct { int kind, face; float strength, pan, dull; } HitRequest;
typedef struct {
    int active, kind, face;
    float time, freq, pan, pan_now, attack;
    float ratio[3], peak[3], envelope[3], fade[3];
    double phase[3];
    float bend, bend_fade;
} Hit;

typedef struct {
    Vertex vertices[VERTICES];
    Vec3 held_vertices[VERTICES];
    Vec3 centre, centre_velocity, shape_centre;
    Vec3 rotation, angular_velocity;
    Vec3 seen_centre, seen_centre_velocity, seen_rotation, seen_angular_velocity;
    Controls controls;
    float energy, time, strike, since_torsion, since_struck[FACES];
    float awake, idle;
    float face_light[FACES], face_fade[FACES], inner_light[FACES], line_flash, beat_phase;
    Vec3 face_colour[FACES];
    float hold_amount;
    int hold_was_active;
    float pose[3];
    float deformation, motion, twist;
    double phase_a, phase_b, phase_c, vibrato_phase;
    float audio_level, audio_frequency, audio_colour, audio_pan, audio_noise;
    float compressor_envelope, compressor_gain, limiter_gain, low_a, low_b;
    float wide[WIDTH_DELAY];
    int wide_index;
    float audio_interval, audio_third, audio_detune, audio_strike;
    float audio_vibrato_depth, audio_vibrato_rate;
    volatile float target_level, target_frequency, target_colour, target_pan, target_noise;
    volatile float target_interval, target_third, target_detune, target_strike;
    volatile float target_vibrato_depth, target_vibrato_rate;
    uint32_t gesture_mask;
    uint32_t noise_state;
    int trigger_signed[2];
    uint32_t buttons;
    int running;
    HitRequest requests[HIT_QUEUE];
    unsigned request_write, request_read;
    Hit hits[HITS];
    float delay_left[LOOKAHEAD], delay_right[LOOKAHEAD];
    int delay_index, quiet;
    float final_gain, final_gain_smooth, final_peak;
    long long limited_samples, total_samples;
} Form;

static uint32_t framebuffer[WIDTH * HEIGHT];
static FILE *capture;
static uint32_t capture_bytes;
static void torsion_pulse(Form *form);
static float clampf(float value, float low, float high);

static void wav_u16(FILE *file, uint16_t value)
{
    fputc(value & 255, file); fputc((value >> 8) & 255, file);
}

static void wav_u32(FILE *file, uint32_t value)
{
    wav_u16(file, value & 65535); wav_u16(file, value >> 16);
}

static void begin_capture(const char *path)
{
    if(!path) return;
    capture_bytes = 0;
    capture = fopen(path, "wb");
    if(!capture) return;
    fwrite("RIFF", 1, 4, capture); wav_u32(capture, 0);
    fwrite("WAVEfmt ", 1, 8, capture); wav_u32(capture, 16);
    wav_u16(capture, 1); wav_u16(capture, 2);
    wav_u32(capture, SAMPLE_RATE); wav_u32(capture, SAMPLE_RATE * 4);
    wav_u16(capture, 4); wav_u16(capture, 16);
    fwrite("data", 1, 4, capture); wav_u32(capture, 0);
}

static void finish_capture(void)
{
    if(!capture) return;
    fseek(capture, 4, SEEK_SET); wav_u32(capture, 36 + capture_bytes);
    fseek(capture, 40, SEEK_SET); wav_u32(capture, capture_bytes);
    fclose(capture); capture = NULL;
}

static Vec3 vec(float x, float y, float z) { Vec3 value = {x, y, z}; return value; }
static Vec3 add(Vec3 a, Vec3 b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec3 sub(Vec3 a, Vec3 b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec3 mul(Vec3 a, float n) { return vec(a.x * n, a.y * n, a.z * n); }
static float length3(Vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static Vec3 normalise(Vec3 a) { float n = length3(a); return n > 0.0001f ? mul(a, 1.0f / n) : vec(0, 0, 0); }

typedef struct { uint32_t seed; float note[6], overtone[6], ring[6], knock_bend, knock_ring, knock_warmth, tick_band, tick_spread, warmth, size; Vec3 rest; } Character;
static Character character = {0x2545f491u, {9.0f / 8.0f, 4.0f / 3.0f, 5.0f / 3.0f, 1.0f, 6.0f / 5.0f, 3.0f / 2.0f},
                                {2.756f, 2.756f, 2.756f, 2.756f, 2.756f, 2.756f}, {1, 1, 1, 1, 1, 1}, 0.35f, 1, 1, 1, 1, 0, 1, {0, 0, 0}};

static float roll(uint32_t *state, float low, float high)
{
    *state = *state * 1664525u + 1013904223u;
    return low + (high - low) * ((*state >> 8) / 16777216.0f);
}

static void roll_character(uint32_t seed)
{
    uint32_t state = seed;
    state ^= state >> 16; state *= 0x7feb352du; state ^= state >> 15; state *= 0x846ca68bu; state ^= state >> 16;
    character.seed = seed;
    {
        static const float seconds[2] = {9.0f / 8.0f, 10.0f / 9.0f}, thirds[2] = {6.0f / 5.0f, 5.0f / 4.0f};
        static const float fifths[2] = {3.0f / 2.0f, 8.0f / 5.0f}, tops[4] = {5.0f / 3.0f, 7.0f / 4.0f, 9.0f / 5.0f, 2.0f};
        character.note[0] = seconds[(int)roll(&state, 0, 1.999f)];
        character.note[1] = 4.0f / 3.0f;
        character.note[2] = tops[(int)roll(&state, 0, 3.999f)];
        character.note[3] = 1.0f;
        character.note[4] = thirds[(int)roll(&state, 0, 1.999f)];
        character.note[5] = fifths[(int)roll(&state, 0, 1.999f)];
        for(int face = 0; face < 6; face++) {
            character.overtone[face] = roll(&state, 2.62f, 2.92f);
            character.ring[face] = roll(&state, 0.9f, 1.1f);
        }
    }
    character.knock_bend = roll(&state, 0.25f, 0.45f);
    character.knock_ring = roll(&state, 0.85f, 1.15f);
    character.knock_warmth = roll(&state, 0.7f, 1.3f);
    character.tick_band = roll(&state, 0.85f, 1.15f);
    character.tick_spread = roll(&state, 0.97f, 1.03f);
    character.warmth = roll(&state, -0.12f, 0.12f);
    character.size = roll(&state, 0.92f, 1.08f);
    character.rest = vec(roll(&state, -0.5f, 0.5f), roll(&state, -3.1f, 3.1f), roll(&state, -0.3f, 0.3f));
}

static float wrap_angle(float angle)
{
    while(angle > PI) angle -= 2.0f * PI;
    while(angle < -PI) angle += 2.0f * PI;
    return angle;
}

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static Vec3 clamp_length(Vec3 value, float maximum)
{
    float n = length3(value);
    return n > maximum ? mul(value, maximum / n) : value;
}

static Vec3 face_point(int face, float u, float v)
{
    switch(face) {
        case 0: return vec( 1,  v, -u);
        case 1: return vec(-1,  v,  u);
        case 2: return vec( u,  1, -v);
        case 3: return vec( u, -1,  v);
        case 4: return vec( u,  v,  1);
        default:return vec(-u,  v, -1);
    }
}

static void initialise(Form *form)
{
    int face, row, column, index = 0;
    memset(form, 0, sizeof(*form));
    for(face = 0; face < FACES; face++) for(row = 0; row < GRID; row++) for(column = 0; column < GRID; column++) {
        float u = -1.0f + 2.0f * column / (GRID - 1);
        float v = -1.0f + 2.0f * row / (GRID - 1);
        form->vertices[index].rest = face_point(face, u, v);
        form->vertices[index].position = form->vertices[index].rest;
        index++;
    }
    form->running = 1;
    form->since_torsion = 10;
    form->awake = 100.0f;
    for(face = 0; face < FACES; face++) form->since_struck[face] = 10;
    form->noise_state = 0x5f3759dfu;
    form->audio_interval = form->target_interval = 1.5f;
    form->audio_third = form->target_third = 2.0f;
    form->compressor_gain = form->limiter_gain = 1.0f;
    form->final_gain = form->final_gain_smooth = 1.0f;
}

static void release_forces(Form *form)
{
    memset(&form->controls, 0, sizeof(form->controls));
}

static void torsion_pulse(Form *form)
{
    int index;
    Vec3 impulses[VERTICES];
    Vec3 mean = vec(0, 0, 0);
    float charge = clampf(form->since_torsion / 1.0f, 0.15f, 1.0f);
    float force = clampf(1.15f - form->deformation * 1.6f, 0.12f, 1.0f) * charge;
    form->since_torsion = 0;
    for(index = 0; index < VERTICES; index++) {
        Vertex *vertex = &form->vertices[index];
        Vec3 local = sub(vertex->position, form->shape_centre);
        Vec3 tangent = normalise(vec(-local.z, 0, local.x));
        float layer = clampf(local.y * 0.72f, -1.0f, 1.0f);
        float strength = 4.15f * force * layer * (0.72f + 0.28f * fabsf(layer));
        if(length3(tangent) < 0.001f) tangent = normalise(vec(-vertex->rest.z, 0, vertex->rest.x));
        impulses[index] = mul(tangent, strength);
        mean = add(mean, impulses[index]);
    }
    mean = mul(mean, 1.0f / VERTICES);
    for(index = 0; index < VERTICES; index++)
        form->vertices[index].velocity = add(form->vertices[index].velocity, sub(impulses[index], mean));
    form->energy = clampf(form->energy + 0.38f, 0, 1);
    form->strike = 1.0f;
}

static const Vec3 strike_screen[4] = {{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}};


static Vec3 object_direction(const Form *form, Vec3 w)
{
    Vec3 angles = add(form->seen_rotation, vec(0.38f, -0.52f, 0.0f));
    float cx = cosf(angles.x), sx = sinf(angles.x);
    float cy = cosf(angles.y), sy = sinf(angles.y);
    float cz = cosf(angles.z), sz = sinf(angles.z);
    Vec3 b = vec(w.x * cz + w.y * sz, -w.x * sz + w.y * cz, w.z);
    Vec3 a = vec(b.x * cy - b.z * sy, b.y, b.x * sy + b.z * cy);
    return vec(a.x, a.y * cx + a.z * sx, -a.y * sx + a.z * cx);
}

static float strike_weight(Vec3 rest, Vec3 direction)
{
    Vec3 n = normalise(rest);
    float facing = n.x * direction.x + n.y * direction.y + n.z * direction.z;
    if(facing <= 0.35f) return 0;
    facing = (facing - 0.35f) / 0.65f;
    return facing * facing;
}

static float pulse_headroom(const Form *form)
{
    return clampf(1.15f - form->deformation * 1.6f, 0.12f, 1.0f);
}

static float body_dullness(const Form *form)
{
    return clampf(form->energy * 0.8f + form->deformation * 1.5f + form->motion * 0.12f, 0, 1);
}

static void request_hit(Form *form, int kind, int face, float strength, float pan)
{
    unsigned write = form->request_write;
    HitRequest *r;
    if(write - __atomic_load_n(&form->request_read, __ATOMIC_ACQUIRE) >= HIT_QUEUE) return;
    r = &form->requests[write % HIT_QUEUE];
    r->kind = kind; r->face = face; r->strength = strength; r->pan = pan; r->dull = body_dullness(form);
    __atomic_store_n(&form->request_write, write + 1, __ATOMIC_RELEASE);
}

static void strike_from(Form *form, Vec3 hit, int kind, float jolt)
{
    Vec3 direction = object_direction(form, hit);
    float headroom = pulse_headroom(form), total = 0, most = -2;
    int struck = 0;
    for(int face = 0; face < FACES; face++) {
        Vec3 n = face_point(face, 0, 0);
        float facing = n.x * direction.x + n.y * direction.y + n.z * direction.z;
        if(facing > most) { most = facing; struck = face; }
    }
    request_hit(form, kind, struck, 0.45f + 0.55f * headroom, hit.x * 0.45f);
    {
        float dull = body_dullness(form), ring = kind == HIT_KNOCK ? 0.16f : 0.42f * (1.0f - 0.45f * dull);
        if(kind == HIT_TICK) form->line_flash = fmaxf(form->line_flash, 0.6f + 0.4f * headroom);
        else {
            form->face_light[struck] = fmaxf(form->face_light[struck], 0.55f + 0.45f * headroom);
            form->face_fade[struck] = ring;
            form->face_colour[struck] = kind == HIT_KNOCK ? vec(1.0f, 0.55f, 0.18f) : vec(0.85f, 0.93f, 1.0f);
        }
    }
    headroom *= clampf(form->since_struck[struck] / 0.3f, 0.25f, 1.0f);
    form->since_struck[struck] = 0;
    if(form->controls.compress) headroom *= DENT_SQUEEZED;
    for(int index = 0; index < VERTICES; index++) total += strike_weight(form->vertices[index].rest, direction);
    total /= VERTICES;
    for(int index = 0; index < VERTICES; index++) {
        Vertex *v = &form->vertices[index];
        float weight = strike_weight(v->rest, direction) - total;
        Vec3 n = normalise(v->rest), p = sub(v->position, form->shape_centre);
        float reach = (p.x * n.x + p.y * n.y + p.z * n.z) / length3(v->rest);
        float give = weight > 0 ? clampf((reach - 0.35f) / 0.55f, 0, 1) : 1;
        v->velocity = sub(v->velocity, mul(direction, 4.2f * weight * headroom * give));
    }
    form->centre_velocity = sub(form->centre_velocity, mul(hit, jolt * (0.5f + 0.5f * pulse_headroom(form))));
    form->seen_centre_velocity = sub(form->seen_centre_velocity, mul(hit, jolt * (0.5f + 0.5f * pulse_headroom(form))));
    form->energy = clampf(form->energy + 0.05f, 0, 1);
    form->idle = 0;
}

static void face_strike(Form *form, int which)
{
    strike_from(form, strike_screen[which], HIT_STRIKE, 1.5f);
}

static void knock_pulse(Form *form)
{
    strike_from(form, vec(0, 0, -1), HIT_KNOCK, 1.2f);
}

static void tick_pulse(Form *form)
{
    strike_from(form, vec(0, 0, 1), HIT_TICK, 0.6f);
}

static int finite_vec(Vec3 value)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static int bit_count(uint32_t value)
{
    int count = 0;
    while(value) { count += value & 1u; value >>= 1; }
    return count;
}

static void simulate(Form *form, float dt)
{
    int index;
    Controls *c = &form->controls;
    float pad_x = (float)(c->rot_right - c->rot_left), pad_y = (float)(c->rot_down - c->rot_up);
    float bend_x = clampf(c->lx - pad_x, -1, 1);
    float bend_y = clampf(c->ly - pad_y, -1, 1);
    float pull = fmaxf(c->pull ? 1.0f : 0.0f, c->ltrigger);
    float push = fmaxf(c->push ? 1.0f : 0.0f, c->rtrigger);
    float extra_damping = c->damping ? 5.0f : 0.0f;
    float radial = (c->expand ? 7.4f : 0.0f) - (c->compress ? 8.2f : 0.0f);
	if(c->damping && !form->hold_was_active)
		for(index = 0; index < VERTICES; index++) form->held_vertices[index] = form->vertices[index].position;
	form->hold_was_active = c->damping;
	form->hold_amount = clampf(form->hold_amount + dt * (c->damping ? 7.0f : -2.2f), 0, 1);
    Vec3 centre_force = vec(bend_x * 4.2f, -bend_y * 4.2f, (pull - push) * 7.0f);
    centre_force = add(centre_force, mul(form->centre, -2.3f));
    centre_force = add(centre_force, mul(form->centre_velocity, -(2.5f + extra_damping)));
    form->centre_velocity = add(form->centre_velocity, mul(centre_force, dt));
    form->centre_velocity = clamp_length(form->centre_velocity, 3.8f);
    form->centre = add(form->centre, mul(form->centre_velocity, dt));
    form->centre = clamp_length(form->centre, 1.20f);
    {
        Vec3 glide = vec(bend_x * 4.2f, -bend_y * 4.2f, (pull - push) * 7.0f);
        glide = add(glide, mul(form->seen_centre, -2.3f));
        glide = add(glide, mul(form->seen_centre_velocity, -(1.3f + extra_damping)));
        form->seen_centre_velocity = clamp_length(add(form->seen_centre_velocity, mul(glide, dt)), 3.8f);
        form->seen_centre = clamp_length(add(form->seen_centre, mul(form->seen_centre_velocity, dt)), 1.20f);
    }

    {
        float rotation_drive = fabsf(c->slx) + fabsf(c->sly) + fabsf(c->rx) + fabsf(c->ry)
                             + c->rot_up + c->rot_down + c->rot_left + c->rot_right;
        Vec3 torque = vec(-c->sly * 4.4f + c->ry * 2.8f,
                          c->slx * 4.4f + c->rx * 2.8f,
                          c->rx * 2.2f);
        if(rotation_drive < 0.04f) torque = add(torque, mul(form->rotation, -1.65f));
        torque = add(torque, mul(form->angular_velocity,
                                 -((rotation_drive < 0.04f ? 2.0f : 0.82f) + extra_damping)));
        form->angular_velocity = add(form->angular_velocity, mul(torque, dt));
        form->angular_velocity = clamp_length(form->angular_velocity, 5.2f);
        form->rotation = add(form->rotation, mul(form->angular_velocity, dt));
        form->rotation.x = wrap_angle(form->rotation.x);
        form->rotation.y = wrap_angle(form->rotation.y);
        form->rotation.z = wrap_angle(form->rotation.z);
        {
            Vec3 spin = vec(-c->sly * 4.4f + c->ry * 2.8f, c->slx * 4.4f + c->rx * 2.8f, c->rx * 2.2f);
            spin = add(spin, mul(form->seen_angular_velocity, -((rotation_drive < 0.04f ? 0.45f : 0.82f) + extra_damping)));
            form->seen_angular_velocity = clamp_length(add(form->seen_angular_velocity, mul(spin, dt)), 5.2f);
            form->seen_rotation = add(form->seen_rotation, mul(form->seen_angular_velocity, dt));
            form->seen_rotation.x = wrap_angle(form->seen_rotation.x);
            form->seen_rotation.y = wrap_angle(form->seen_rotation.y);
            form->seen_rotation.z = wrap_angle(form->seen_rotation.z);
        }
    }

    for(index = 0; index < VERTICES; index++) {
        Vertex *vertex = &form->vertices[index];
        Vec3 displacement = sub(vertex->position, vertex->rest);
        Vec3 force = add(mul(displacement, -(8.5f * (1.0f - form->hold_amount * 0.72f))),
                         mul(vertex->velocity, -(3.1f + extra_damping)));
        Vec3 outward = normalise(sub(vertex->position, form->shape_centre));
        if(length3(outward) < 0.001f) outward = normalise(vertex->rest);
		if(form->hold_amount > 0)
			force = add(force, mul(sub(vertex->position, form->held_vertices[index]), -18.0f * form->hold_amount));
        float edge_x = 0.35f + 0.65f * fabsf(vertex->rest.x);
        float edge_y = 0.35f + 0.65f * fabsf(vertex->rest.y);
        force.x += bend_x * edge_x * 5.4f;
        force.y -= bend_y * edge_y * 5.4f;
        force.x += c->rx * vertex->rest.y * 4.8f;
        force.z -= c->rx * vertex->rest.x * 4.2f;
        force.y += c->ry * vertex->rest.z * 4.5f;
        force.z += (pull - push) * (0.9f + fabsf(vertex->rest.z)) * 3.8f;
        force = add(force, mul(outward, radial));
        if(c->resonance) {
            float wave = sinf(form->time * 12.0f + vertex->rest.x * 2.1f + vertex->rest.y * 1.7f);
            force = add(force, mul(outward, wave * 3.4f));
        }
        vertex->velocity = add(vertex->velocity, mul(force, dt));
        vertex->velocity = clamp_length(vertex->velocity, 8.5f);
        vertex->position = add(vertex->position, mul(vertex->velocity, dt));
        displacement = clamp_length(sub(vertex->position, vertex->rest), 1.55f);
        vertex->position = add(vertex->rest, displacement);
        {
            Vec3 n = normalise(vertex->rest), p = sub(vertex->position, form->shape_centre);
            float rest_length = length3(vertex->rest);
            float reach = (p.x * n.x + p.y * n.y + p.z * n.z) / rest_length;
            if(reach < 0.32f) {
                float inward = vertex->velocity.x * n.x + vertex->velocity.y * n.y + vertex->velocity.z * n.z;
                float closeness = clampf((0.32f - reach) / 0.2f, 0, 1);
                if(inward < 0) vertex->velocity = sub(vertex->velocity, mul(n, inward * closeness));
                if(reach < 0.12f) vertex->position = add(vertex->position, mul(n, (0.12f - reach) * rest_length));
            }
        }
        if(!finite_vec(vertex->position) || !finite_vec(vertex->velocity)) {
            vertex->position = vertex->rest;
            vertex->velocity = vec(0, 0, 0);
        }
    }
    form->shape_centre = vec(0, 0, 0);
    for(index = 0; index < VERTICES; index++)
        form->shape_centre = add(form->shape_centre, form->vertices[index].position);
    form->shape_centre = mul(form->shape_centre, 1.0f / VERTICES);
    if(c->colour) form->energy += dt * 0.75f;
    if(c->resonance) form->energy += dt * 0.22f;
    form->energy += dt * (pull + push) * 0.32f;
    form->energy = clampf(form->energy - dt * 0.14f, 0, 1);
    form->strike = fmaxf(0.0f, form->strike - dt * 2.8f);
    {
        float deformation = 0, motion = 0, radius = 0;
        for(index = 0; index < VERTICES; index++) {
            deformation += length3(sub(form->vertices[index].position, form->vertices[index].rest));
            motion += length3(form->vertices[index].velocity);
            radius += length3(form->vertices[index].position) - length3(form->vertices[index].rest);
        }
        form->deformation = deformation / VERTICES;
        form->motion = motion / VERTICES + length3(form->centre_velocity) * 0.4f + length3(form->angular_velocity) * 0.25f;
        form->twist = clampf(length3(form->rotation) + fabsf(c->rx) * 0.35f + fabsf(c->ry) * 0.2f, 0, 1.5f);
        uint32_t mask = 0;
        float gesture = fabsf(bend_x) + fabsf(bend_y) + fabsf(c->slx) + fabsf(c->sly) + fabsf(c->rx) + fabsf(c->ry);
        gesture += c->compress + c->expand + c->disturb + c->colour;
        gesture += c->damping + c->resonance + pull + push;
        if(c->expand) mask |= 1u;
        if(c->compress) mask |= 2u;
        if(c->disturb) mask |= 4u;
        if(c->colour) mask |= 8u;
        if(c->damping) mask |= 16u;
        if(c->resonance) mask |= 32u;
        if(pull > 0.08f) mask |= 64u;
        if(push > 0.08f) mask |= 128u;
        if(fabsf(bend_x) + fabsf(bend_y) > 0.15f) mask |= 256u;
        if(fabsf(c->slx) + fabsf(c->sly) > 0.15f) mask |= 512u;
        if(fabsf(c->rx) + fabsf(c->ry) > 0.15f) mask |= 1024u;
        if(mask != form->gesture_mask) {
            if(mask) form->strike = fmaxf(form->strike, c->disturb ? 1.0f : 0.34f);
            form->gesture_mask = mask;
        }
        int voices = bit_count(mask);
        float midi = 52.0f - c->sly * 12.0f + c->slx * 4.0f - bend_y * 5.0f + bend_x * 2.0f;
        midi += (c->expand ? 5.0f : 0.0f) - (c->compress ? 5.0f : 0.0f) + (pull - push) * 3.0f;
        midi = midi * 0.68f + roundf(midi) * 0.32f;
        form->target_level = form->motion * 0.15f + form->deformation * 0.13f + form->energy * 0.07f + gesture * 0.022f + (mask ? 0.10f : 0);
        form->target_level *= 0.88f + pull * PULL_LIFT - push * 0.16f;
        form->idle = mask ? 0 : form->idle + dt;
        form->target_level *= expf(-form->idle / RELEASE);
        form->target_level = clampf(form->target_level, 0, 0.46f);
        if(form->target_level < 0.0015f) form->target_level = 0;
        form->target_frequency = clampf(440.0f * powf(2.0f, (midi - 69.0f) / 12.0f), 70, 620);
		if(c->damping) {
			form->target_level *= 0.72f;
			form->target_frequency *= 0.82f;
		}
        form->target_colour = clampf(0.16f + form->twist * 0.18f + (c->resonance ? 0.62f : 0) + (c->colour ? 0.24f : 0) - (c->damping ? 0.24f : 0), 0, 1);
        form->target_noise = clampf(form->motion * 0.004f, 0, 0.025f);
        form->target_interval = c->disturb ? 1.2f : c->expand ? 1.50f : c->compress ? 1.066667f : c->resonance ? 2.0f : 1.0035f;
        form->target_third = c->disturb ? 1.6f : c->colour ? 1.5f : c->resonance ? 2.002f : 1.0068f;
        if(c->expand && c->compress) { form->target_interval = 1.333333f; form->target_third = 1.666667f; }
        if(c->colour && c->resonance) { form->target_interval = 1.4f; form->target_third = 2.25f; }
        if(pull > 0.5f && push > 0.5f) { form->target_interval = 1.20f; form->target_third = 1.80f; }
        form->target_detune = (c->disturb ? 0.0090f : c->colour ? 0.0065f : 0.0018f) * fminf(voices, 4);
        form->target_vibrato_depth = clampf(fabsf(c->rx) * 0.38f + form->strike * 0.22f + (c->colour ? 0.14f : 0), 0.04f, 0.6f);
        form->target_vibrato_rate = clampf(0.7f + c->ry * 0.5f + (c->resonance ? 0.25f : 0), 0.25f, 1.4f);
        form->target_strike = form->strike;
        form->target_pan = clampf(form->centre.x * 0.65f, -0.72f, 0.72f);
        {
            int held[3] = {c->expand && c->compress, c->colour && c->resonance, pull > 0.5f && push > 0.5f};
            float voice = clampf(form->audio_level / 0.18f, 0, 1);
            for(int k = 0; k < 3; k++) form->pose[k] += ((held[k] ? voice : 0.0f) - form->pose[k]) * clampf(dt * 3.0f, 0, 1);
        }
    }
    form->since_torsion += dt;
    if(form->awake < 100.0f) form->awake += dt;
    for(index = 0; index < FACES; index++) {
        form->since_struck[index] += dt;
        if(form->face_light[index] > 0) {
            form->face_light[index] *= expf(-dt / fmaxf(form->face_fade[index], 0.05f));
            if(form->face_light[index] < 0.003f) form->face_light[index] = 0;
        }
    }
    form->line_flash *= expf(-dt / 0.12f);
    for(index = 0; index < FACES; index++) form->inner_light[index] += (form->face_light[index] - form->inner_light[index]) * clampf(dt * 10.0f, 0, 1);
    {
        float ratio = form->target_interval * (1.0f + form->target_detune);
        float beat = form->target_frequency * fabsf(ratio - 1.0f);
        if(beat < 6.0f) form->beat_phase += beat * dt;
        if(form->beat_phase > 1000.0f) form->beat_phase -= 1000.0f;
    }
    form->time += dt;
}

typedef struct { float cx, sx, cy, sy, cz, sz; Vec3 shape_centre, centre; } View;
typedef struct { int x0, y0, x1, y1; } Area;
static Area previous_area = {0, 0, WIDTH - 1, HEIGHT - 1};

static void area_add(Area *area, int x0, int y0, int x1, int y1)
{
    if(x0 > x1 || y0 > y1) return;
    if(area->x0 > area->x1) { area->x0 = x0; area->y0 = y0; area->x1 = x1; area->y1 = y1; return; }
    if(x0 < area->x0) area->x0 = x0;
    if(y0 < area->y0) area->y0 = y0;
    if(x1 > area->x1) area->x1 = x1;
    if(y1 > area->y1) area->y1 = y1;
}

static void render_reset(void)
{
    previous_area.x0 = 0; previous_area.y0 = 0;
    previous_area.x1 = WIDTH - 1; previous_area.y1 = HEIGHT - 1;
    for(int index = 0; index < WIDTH * HEIGHT; index++) framebuffer[index] = 0xff000000u;
}

static View make_view(Form *form)
{
    View view;
    Vec3 angles = add(form->seen_rotation, vec(0.38f, -0.52f, 0.0f));
    view.cx = cosf(angles.x); view.sx = sinf(angles.x);
    view.cy = cosf(angles.y); view.sy = sinf(angles.y);
    view.cz = cosf(angles.z); view.sz = sinf(angles.z);
    view.shape_centre = form->shape_centre;
    view.centre = form->seen_centre;
    return view;
}

static Vec3 rotate_view(Vec3 point, const View *v)
{
    Vec3 a = vec(point.x, point.y * v->cx - point.z * v->sx, point.y * v->sx + point.z * v->cx);
    Vec3 b = vec(a.x * v->cy + a.z * v->sy, a.y, -a.x * v->sy + a.z * v->cy);
    return vec(b.x * v->cz - b.y * v->sz, b.x * v->sz + b.y * v->cz, b.z);
}

static ScreenVertex project(const View *view, Vec3 point)
{
    Vec3 local = sub(point, view->shape_centre);
    Vec3 world = add(add(rotate_view(local, view), view->shape_centre), view->centre);
    float depth = 4.5f + world.z;
    float scale = 200.0f * VIEW_SIZE / depth;
    ScreenVertex result = {WIDTH * 0.5f + world.x * scale, HEIGHT * 0.5f - world.y * scale, depth, world};
    return result;
}

static Vec3 cross3(Vec3 a, Vec3 b)
{
    return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static uint32_t grey(float level)
{
    int value = (int)(clampf(level, 0, 1) * 250);
    return ((uint32_t)value << 16) | ((uint32_t)value << 8) | (uint32_t)value;
}

#define BADGE_TIME 2.7f
typedef struct { int arc; float x0, y0, x1, y1, r, a0, a1; } Stroke;
#define BADGE_X0 150
#define BADGE_Y0 170
#define BADGE_W 340
#define BADGE_H 150
static float badge_alpha[BADGE_H][BADGE_W];
static int badge_ready;

static float segment_distance(float px, float py, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0, length = dx * dx + dy * dy;
    float t = length > 0 ? clampf(((px - x0) * dx + (py - y0) * dy) / length, 0, 1) : 0;
    float ex = px - (x0 + dx * t), ey = py - (y0 + dy * t);
    return sqrtf(ex * ex + ey * ey);
}

static float arc_distance(float px, float py, const Stroke *s)
{
    float dx = px - s->x0, dy = s->y0 - py;
    float angle = atan2f(dy, dx), span = s->a1 - s->a0;
    float relative = fmodf(angle - s->a0 + 4.0f * PI, 2.0f * PI);
    if(relative <= span || span >= 2.0f * PI - 0.001f) return fabsf(sqrtf(dx * dx + dy * dy) - s->r);
    {
        float ax = s->x0 + s->r * cosf(s->a0), ay = s->y0 - s->r * sinf(s->a0);
        float bx = s->x0 + s->r * cosf(s->a1), by = s->y0 - s->r * sinf(s->a1);
        float da = sqrtf((px - ax) * (px - ax) + (py - ay) * (py - ay));
        float db = sqrtf((px - bx) * (px - bx) + (py - by) * (py - by));
        return fminf(da, db);
    }
}

static int letter(char c, float x, float base, float h, Stroke *out, float *advance)
{
    int n = 0;
    float deg = PI / 180.0f;
#define SEG(ax, ay, bx, by) (out[n].arc = 0, out[n].x0 = x + (ax) * h, out[n].y0 = base - (ay) * h, out[n].x1 = x + (bx) * h, out[n].y1 = base - (by) * h, n++)
#define ARC(cx, cy, rr, from, to) (out[n].arc = 1, out[n].x0 = x + (cx) * h, out[n].y0 = base - (cy) * h, out[n].r = (rr) * h, out[n].a0 = (from) * deg, out[n].a1 = (to) * deg, n++)
    switch(c) {
        case 'o': ARC(0.5f, 0.5f, 0.5f, 0, 360); *advance = 1.0f; break;
        case 'c': ARC(0.5f, 0.5f, 0.5f, 40, 320); *advance = 0.95f; break;
        case 'e': SEG(0, 0.5f, 1, 0.5f); ARC(0.5f, 0.5f, 0.5f, 0, 320); *advance = 1.0f; break;
        case 'a': ARC(0.5f, 0.5f, 0.5f, 0, 360); SEG(1, 0, 1, 1); *advance = 1.0f; break;
        case 'd': ARC(0.5f, 0.5f, 0.5f, 0, 360); SEG(1, 0, 1, 1.55f); *advance = 1.0f; break;
        case 'p': ARC(0.5f, 0.5f, 0.5f, 0, 360); SEG(0, 1, 0, -0.55f); *advance = 1.0f; break;
        case 'n': SEG(0, 0, 0, 1); ARC(0.45f, 0.55f, 0.45f, 0, 180); SEG(0.9f, 0.55f, 0.9f, 0); *advance = 0.9f; break;
        case 'h': SEG(0, 0, 0, 1.55f); ARC(0.45f, 0.55f, 0.45f, 0, 180); SEG(0.9f, 0.55f, 0.9f, 0); *advance = 0.9f; break;
        case 'm': SEG(0, 0, 0, 1); ARC(0.37f, 0.6f, 0.37f, 0, 180); SEG(0.74f, 0.6f, 0.74f, 0);
                  ARC(1.11f, 0.6f, 0.37f, 0, 180); SEG(1.48f, 0.6f, 1.48f, 0); *advance = 1.48f; break;
        case 'r': SEG(0, 0, 0, 1); ARC(0.42f, 0.55f, 0.42f, 60, 180); *advance = 0.6f; break;
        case 's': ARC(0.4f, 0.74f, 0.26f, 30, 270); ARC(0.4f, 0.26f, 0.26f, 210, 450); *advance = 0.8f; break;
        default: *advance = 0.25f; break;
    }
#undef SEG
#undef ARC
    return n;
}

static void stroke_into(const Stroke *s, int count, float width, float level)
{
    float reach = width * 0.5f + 1.0f;
    for(int i = 0; i < count; i++) {
        float left = s[i].arc ? s[i].x0 - s[i].r : fminf(s[i].x0, s[i].x1), right = s[i].arc ? s[i].x0 + s[i].r : fmaxf(s[i].x0, s[i].x1);
        float top = s[i].arc ? s[i].y0 - s[i].r : fminf(s[i].y0, s[i].y1), bottom = s[i].arc ? s[i].y0 + s[i].r : fmaxf(s[i].y0, s[i].y1);
        int x0 = (int)(left - reach) - BADGE_X0, x1 = (int)(right + reach) - BADGE_X0 + 1;
        int y0 = (int)(top - reach) - BADGE_Y0, y1 = (int)(bottom + reach) - BADGE_Y0 + 1;
        if(x0 < 0) x0 = 0;
        if(y0 < 0) y0 = 0;
        if(x1 > BADGE_W) x1 = BADGE_W;
        if(y1 > BADGE_H) y1 = BADGE_H;
        for(int y = y0; y < y1; y++) for(int x = x0; x < x1; x++) {
            float px = BADGE_X0 + x + 0.5f, py = BADGE_Y0 + y + 0.5f;
            float d = s[i].arc ? arc_distance(px, py, &s[i]) : segment_distance(px, py, s[i].x0, s[i].y0, s[i].x1, s[i].y1);
            float alpha = clampf(width * 0.5f + 0.5f - d, 0, 1) * level;
            if(alpha > badge_alpha[y][x]) badge_alpha[y][x] = alpha;
        }
    }
}

static void prepare_badge(void)
{
    static Stroke strokes[160];
    const char *lines[2] = {"headphones", "recommended"};
    float h = 9.0f, gap = 0.34f, x, advance;
    int n = 0;
    float cx = WIDTH * 0.5f, top = 222.0f, band = 25.0f;
    strokes[n++] = (Stroke){1, cx, top, 0, 0, band, 0, PI};
    for(int side = -1; side <= 1; side += 2) {
        float ex = cx + side * band;
        strokes[n++] = (Stroke){0, ex, top, ex, top + 4.0f, 0, 0, 0};
        strokes[n++] = (Stroke){1, ex, top + 9.0f, 0, 0, 5.0f, 0, PI};
        strokes[n++] = (Stroke){1, ex, top + 21.0f, 0, 0, 5.0f, PI, 2.0f * PI};
        strokes[n++] = (Stroke){0, ex - 5.0f, top + 9.0f, ex - 5.0f, top + 21.0f, 0, 0, 0};
        strokes[n++] = (Stroke){0, ex + 5.0f, top + 9.0f, ex + 5.0f, top + 21.0f, 0, 0, 0};
    }
    stroke_into(strokes, n, 1.6f, 0.85f);
    n = 0;
    for(int line = 0; line < 2; line++) {
        float width = 0;
        for(const char *c = lines[line]; *c; c++) { Stroke scratch[8]; letter(*c, 0, 0, h, scratch, &advance); width += (advance + gap) * h; }
        width -= gap * h;
        x = WIDTH * 0.5f - width * 0.5f;
        for(const char *c = lines[line]; *c; c++) { n += letter(*c, x, 278.0f + line * 24.0f, h, strokes + n, &advance); x += (advance + gap) * h; }
    }
    stroke_into(strokes, n, 1.3f, 0.6f);
    badge_ready = 1;
}

static void draw_badge(float remaining, Area *drawn)
{
    float t = (1.0f - remaining) * BADGE_TIME, fade;
    if(!badge_ready) prepare_badge();
    fade = t < 0.6f ? t / 0.6f : t < 1.9f ? 1.0f : clampf((2.5f - t) / 0.6f, 0, 1);
    fade = fade * fade * (3.0f - 2.0f * fade);
    if(fade <= 0) return;
    for(int y = 0; y < BADGE_H; y++) for(int x = 0; x < BADGE_W; x++) {
        float a = badge_alpha[y][x] * fade;
        if(a > 0.004f) framebuffer[(BADGE_Y0 + y) * WIDTH + BADGE_X0 + x] = 0xff000000u | grey(a);
    }
    area_add(drawn, BADGE_X0, BADGE_Y0, BADGE_X0 + BADGE_W - 1, BADGE_Y0 + BADGE_H - 1);
}

static Vec3 drawn_point(const Form *form, int index)
{
    const Vertex *v = &form->vertices[index];
    int face = index / (GRID * GRID), row = index / GRID % GRID, column = index % GRID;
    float u = -1.0f + 2.0f * column / (GRID - 1), w = -1.0f + 2.0f * row / (GRID - 1);
    Vec3 normal = face_point(face, 0, 0), n = normalise(v->rest), p, local;
    Vec3 moved = clamp_length(mul(sub(v->position, v->rest), EXAGGERATE), 1.9f);
    float reach, rest_length = length3(v->rest);
    p = add(v->rest, moved);
    if(form->pose[0] > 0.001f)
        p = add(p, mul(normal, form->pose[0] * 0.30f * cosf(PI * 0.5f * u) * cosf(PI * 0.5f * w) * sinf(form->time * 6.9f)));
    if(form->pose[1] > 0.001f)
        p = add(p, mul(normal, form->pose[1] * 0.24f * sinf(PI * u) * sinf(PI * w) * (0.85f + 0.15f * sinf(form->time * 9.0f))));
    if(form->pose[2] > 0.001f) {
        float squash = form->pose[2];
        p = vec(p.x * (1.0f + 0.16f * squash), p.y * (1.0f - 0.32f * squash), p.z * (1.0f + 0.16f * squash));
    }
    local = sub(p, form->shape_centre);
    reach = (local.x * n.x + local.y * n.y + local.z * n.z) / rest_length;
    if(reach < 0.2f) p = add(p, mul(n, (0.2f - reach) * rest_length));
    return p;
}

static void plot_point(float px, float py, float radius, float glow, Vec3 c, Area *drawn)
{
    float reach = radius * 3.0f + 1.0f, inverse = 1.0f / (reach * reach), edge = (radius + 0.5f) * (radius + 0.5f), soft = 1.0f / (2.0f * radius + 1.0f);
    int x0 = (int)(px - reach), x1 = (int)(px + reach) + 1, y0 = (int)(py - reach), y1 = (int)(py + reach) + 1;
    int cr = (int)(c.x * 255), cg = (int)(c.y * 255), cb = (int)(c.z * 255);
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > WIDTH - 1) x1 = WIDTH - 1;
    if(y1 > HEIGHT - 1) y1 = HEIGHT - 1;
    if(x0 > x1 || y0 > y1) return;
    area_add(drawn, x0, y0, x1, y1);
    for(int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - py;
        uint32_t *row = framebuffer + y * WIDTH;
        for(int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - px, d2 = dx * dx + dy * dy, halo = 1.0f - d2 * inverse, w;
            int k, r, g, b;
            if(halo <= 0) continue;
            w = clampf((edge - d2) * soft, 0, 1) + halo * halo * glow;
            k = (int)(w * 256);
            r = (int)(row[x] >> 16 & 255) + (cr * k >> 8);
            g = (int)(row[x] >> 8 & 255) + (cg * k >> 8);
            b = (int)(row[x] & 255) + (cb * k >> 8);
            row[x] = 0xff000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
        }
    }
}

static Area draw_frame(Form *form)
{
    Area drawn = {1, 1, 0, 0}, changed;
    View view = make_view(form);
    float voice = clampf(form->audio_level / 0.25f, 0, 1), warmth = clampf(body_dullness(form) + character.warmth, -0.12f, 1);
    float vibrato = sinf((float)(form->vibrato_phase * 6.283185307179586)) * clampf(form->audio_vibrato_depth / 0.4f, 0, 1);
    float pulse = 1.0f + 0.25f * vibrato + 0.2f * cosf(form->beat_phase * 6.2831853f);
    float reveal = clampf(form->awake / 2.5f, 0, 1);
    Vec3 white = add(mul(vec(0.72f, 0.82f, 1.0f), 1.0f - warmth), mul(vec(1.0f, 0.80f, 0.56f), warmth));
    if(previous_area.x0 <= previous_area.x1)
        for(int y = previous_area.y0; y <= previous_area.y1; y++)
            for(int x = previous_area.x0; x <= previous_area.x1; x++) framebuffer[y * WIDTH + x] = 0xff000000u;
    if(form->awake < 0) draw_badge(clampf(-form->awake / BADGE_TIME, 0, 1), &drawn);
    else {
        static Vec3 shaped[VERTICES];
        for(int index = 0; index < VERTICES; index++) shaped[index] = drawn_point(form, index);
        for(int index = 0; index < VERTICES; index++) {
            int face = index / (GRID * GRID);
            ScreenVertex p = project(&view, shaped[index]);
            float near = clampf(1.0f - (p.depth - 4.5f) * 0.32f, 0.3f, 1.6f);
            float order = (float)((index * 37) % VERTICES) / VERTICES, shown = clampf(reveal * 1.6f - order * 0.6f, 0, 1);
            float level = (0.32f + voice * 0.55f * pulse + form->line_flash * 0.8f) * near;
            float radius = (0.8f + 0.35f * near + 0.25f * form->face_light[face] + 0.3f * voice) * VIEW_SIZE / 1.6f * character.size;
            Vec3 colour = add(mul(white, fminf(level, 0.8f)), mul(form->face_colour[face], form->face_light[face] * 0.4f * near));
            if(shown <= 0) continue;
            plot_point(p.x, p.y, radius * (0.5f + 0.5f * shown), 0.3f + 0.6f * voice + 1.2f * form->face_light[face] + 0.6f * form->line_flash, mul(colour, shown), &drawn);
        }
        for(int i = 0; i < INNER_POINTS; i++) {
            static float iu[INNER_POINTS], iv[INNER_POINTS], idepth[INNER_POINTS];
            static int iface[INNER_POINTS], ready;
            float gx, gy, tx, ty, depth;
            int face, c0, r0, base;
            Vec3 surface;
            if(!ready) {
                uint32_t seed = character.seed;
                for(int k = 0; k < INNER_POINTS; k++) {
                    seed = seed * 1664525u + 1013904223u; iface[k] = (int)(seed >> 8) % FACES;
                    seed = seed * 1664525u + 1013904223u; iu[k] = (seed >> 8) / 16777216.0f * 1.8f - 0.9f;
                    seed = seed * 1664525u + 1013904223u; iv[k] = (seed >> 8) / 16777216.0f * 1.8f - 0.9f;
                    seed = seed * 1664525u + 1013904223u; idepth[k] = 0.15f + 0.72f * cbrtf((seed >> 8) / 16777216.0f);
                }
                ready = 1;
            }
            face = iface[i]; depth = idepth[i];
            gx = (iu[i] + 1.0f) * 0.5f * (GRID - 1); gy = (iv[i] + 1.0f) * 0.5f * (GRID - 1);
            c0 = (int)gx; r0 = (int)gy; tx = gx - c0; ty = gy - r0;
            base = face * GRID * GRID + r0 * GRID + c0;
            surface = add(mul(add(mul(shaped[base], 1 - tx), mul(shaped[base + 1], tx)), 1 - ty),
                          mul(add(mul(shaped[base + GRID], 1 - tx), mul(shaped[base + GRID + 1], tx)), ty));
            {
                ScreenVertex p = project(&view, add(form->shape_centre, mul(sub(surface, form->shape_centre), depth)));
                float near = clampf(1.0f - (p.depth - 4.5f) * 0.32f, 0.3f, 1.6f);
                float order = (float)((i * 53 + 17) % INNER_POINTS) / INNER_POINTS, shown = clampf(reveal * 1.6f - order * 0.6f - 0.15f, 0, 1);
                float light = form->face_light[face] * depth + form->inner_light[face] * (1.0f - depth), fade = depth * sqrtf(depth);
                float level = (0.32f + voice * 0.55f * pulse + form->line_flash * 0.6f) * near * fade;
                float radius = (0.55f + 0.5f * depth) * (0.8f + 0.35f * near + 0.25f * light + 0.3f * voice) * VIEW_SIZE / 1.6f * character.size;
                Vec3 colour = add(mul(white, fminf(level, 0.8f)), mul(form->face_colour[face], light * 0.4f * near * fade));
                if(shown <= 0) continue;
                plot_point(p.x, p.y, radius * (0.5f + 0.5f * shown), (0.3f + 0.6f * voice + 1.0f * light) * fade, mul(colour, shown), &drawn);
            }
        }
    }
    changed = drawn;
    if(previous_area.x0 <= previous_area.x1) area_add(&changed, previous_area.x0, previous_area.y0, previous_area.x1, previous_area.y1);
    previous_area = drawn;
    return changed;
}

static void render(Form *form, SDL_Renderer *renderer, SDL_Texture *texture)
{
    Area changed = draw_frame(form);
    if(changed.x0 <= changed.x1) {
        SDL_Rect rect = {changed.x0, changed.y0, changed.x1 - changed.x0 + 1, changed.y1 - changed.y0 + 1};
        SDL_UpdateTexture(texture, &rect, framebuffer + changed.y0 * WIDTH + changed.x0, WIDTH * (int)sizeof(uint32_t));
    }
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static void start_hit(Form *form, const HitRequest *r)
{
    Hit *h = NULL;
    float root = form->target_frequency, dull = r->dull, tau[3] = {0, 0, 0}, level[3] = {0, 0, 0};
    for(int i = 0; i < HITS && !h; i++)
        if(form->hits[i].active && form->hits[i].kind == r->kind && form->hits[i].face == r->face) h = &form->hits[i];
    if(!h) {
        for(int i = 0; i < HITS && !h; i++) if(!form->hits[i].active) h = &form->hits[i];
        if(!h) {
            float quietest = 1e9f;
            for(int i = 0; i < HITS; i++) {
                float level = form->hits[i].envelope[0] + form->hits[i].envelope[1] + form->hits[i].envelope[2];
                if(level < quietest) { quietest = level; h = &form->hits[i]; }
            }
        }
        memset(h, 0, sizeof(*h));
        h->pan_now = r->pan;
    }
    if(root < 70) root = 70;
    h->active = 1; h->kind = r->kind; h->face = r->face; h->pan = r->pan; h->time = 0;
    switch(r->kind) {
        case HIT_KNOCK:
            h->freq = root * 0.5f;
            h->ratio[0] = 1; level[0] = 1.0f / sqrtf(character.knock_ring); tau[0] = 0.16f * character.knock_ring;
            h->ratio[1] = 2; level[1] = 0.18f * character.knock_warmth * (1 - dull); tau[1] = 0.05f;
            h->ratio[2] = 3.01f; level[2] = 0.05f; tau[2] = 0.015f;
            h->bend = character.knock_bend; h->bend_fade = expf(-1.0f / (0.025f * SAMPLE_RATE));
            break;
        case HIT_TICK:
            h->freq = root * 4.0f * character.note[r->face] * character.tick_band;
            while(h->freq > 3200) h->freq *= 0.5f;
            while(h->freq < 1600) h->freq *= 2.0f;
            h->ratio[0] = 1; level[0] = 0.55f; tau[0] = 0.018f;
            h->ratio[1] = 1.483f * character.tick_spread; level[1] = 0.35f; tau[1] = 0.012f;
            h->ratio[2] = 2.137f / character.tick_spread; level[2] = 0.2f * (1 - 0.6f * dull); tau[2] = 0.007f;
            h->bend_fade = 1;
            break;
        default:
            h->freq = root * 2.0f * character.note[r->face];
            while(h->freq > 1000) h->freq *= 0.5f;
            h->ratio[0] = 1; level[0] = 1.0f / sqrtf(character.ring[r->face]); tau[0] = 0.42f * character.ring[r->face] * (1.0f - 0.45f * dull);
            h->ratio[1] = character.overtone[r->face]; level[1] = 0.22f * (1.0f - 0.7f * dull); tau[1] = 0.09f;
            h->ratio[2] = 5.404f; level[2] = 0.07f * (1.0f - 0.9f * dull); tau[2] = 0.02f;
            h->bend = 0.004f; h->bend_fade = expf(-1.0f / (0.01f * SAMPLE_RATE));
            break;
    }
    for(int k = 0; k < 3; k++) {
        h->peak[k] = level[k] * r->strength;
        h->fade[k] = tau[k] > 0 ? expf(-1.0f / (tau[k] * SAMPLE_RATE)) : 0;
    }
    h->attack = 1.0f - expf(-1.0f / (0.0007f * SAMPLE_RATE));
}

static float hit_sample(Hit *h)
{
    float y = 0, pitch = h->freq * (1.0f + h->bend), quiet = 1;
    int rising = h->time < 0.004f;
    for(int k = 0; k < 3; k++) {
        if(h->peak[k] <= 0 && h->envelope[k] <= 0) continue;
        if(rising) h->envelope[k] += (h->peak[k] - h->envelope[k]) * h->attack;
        else { h->envelope[k] *= h->fade[k]; h->peak[k] = 0; }
        h->phase[k] += pitch * h->ratio[k] / SAMPLE_RATE;
        if(h->phase[k] >= 1) h->phase[k] -= 1;
        y += h->envelope[k] * sinf((float)(h->phase[k] * 6.283185307179586));
        if(h->envelope[k] < 0.0002f && !rising) h->envelope[k] = 0; else quiet = 0;
    }
    h->bend *= h->bend_fade;
    h->time += 1.0f / SAMPLE_RATE;
    if(quiet) h->active = 0;
    return y;
}

static void audio_callback(void *userdata, Uint8 *stream, int bytes)
{
    Form *form = userdata;
    int16_t *samples = (int16_t *)stream;
    int frames = bytes / (int)(sizeof(int16_t) * 2);
    {
        unsigned write = __atomic_load_n(&form->request_write, __ATOMIC_ACQUIRE);
        while(form->request_read != write) {
            start_hit(form, &form->requests[form->request_read % HIT_QUEUE]);
            __atomic_store_n(&form->request_read, form->request_read + 1, __ATOMIC_RELEASE);
        }
    }
    {
        int ringing = 0;
        for(int i = 0; i < HITS; i++) ringing |= form->hits[i].active;
        if(form->quiet && !ringing && form->audio_level == 0 && form->target_level == 0) {
            float n = (float)frames;
#define GLIDE(value, target, rate) (value += ((target) - (value)) * (1.0f - powf(1.0f - (rate), n)))
            GLIDE(form->audio_frequency, form->target_frequency, 0.00058f);
            GLIDE(form->audio_colour, form->target_colour, 0.0010f);
            GLIDE(form->audio_pan, form->target_pan, 0.0015f);
            GLIDE(form->audio_noise, form->target_noise, 0.0030f);
            GLIDE(form->audio_interval, form->target_interval, 0.0018f);
            GLIDE(form->audio_third, form->target_third, 0.0015f);
            GLIDE(form->audio_detune, form->target_detune, 0.0012f);
            GLIDE(form->audio_strike, form->target_strike, form->target_strike > form->audio_strike ? 0.018f : 0.0025f);
            GLIDE(form->audio_vibrato_depth, form->target_vibrato_depth, 0.0012f);
            GLIDE(form->audio_vibrato_rate, form->target_vibrato_rate, 0.0010f);
#undef GLIDE
            memset(stream, 0, (size_t)bytes);
            if(capture) { fwrite(stream, 1, (size_t)bytes, capture); capture_bytes += (uint32_t)bytes; }
            return;
        }
    }
    form->quiet = 1;
    for(int frame = 0; frame < frames; frame++) {
        float attack = form->target_level > form->audio_level ? 0.0070f : 0.0010f;
        float fundamental, harmonic, air, noise, signal, left, right, vibrato, instant_frequency;
        float peak, compressed_peak, target_compressor_gain, target_limiter_gain;
        form->audio_level += (form->target_level - form->audio_level) * attack;
        form->audio_frequency += (form->target_frequency - form->audio_frequency) * 0.00058f;
        form->audio_colour += (form->target_colour - form->audio_colour) * 0.0010f;
        form->audio_pan += (form->target_pan - form->audio_pan) * 0.0015f;
        form->audio_noise += (form->target_noise - form->audio_noise) * 0.0030f;
        form->audio_interval += (form->target_interval - form->audio_interval) * 0.0018f;
        form->audio_third += (form->target_third - form->audio_third) * 0.0015f;
        form->audio_detune += (form->target_detune - form->audio_detune) * 0.0012f;
        form->audio_strike += (form->target_strike - form->audio_strike) * (form->target_strike > form->audio_strike ? 0.018f : 0.0025f);
        form->audio_vibrato_depth += (form->target_vibrato_depth - form->audio_vibrato_depth) * 0.0012f;
        form->audio_vibrato_rate += (form->target_vibrato_rate - form->audio_vibrato_rate) * 0.0010f;
        if(form->audio_level < 0.00008f && form->target_level == 0) form->audio_level = 0;
        form->vibrato_phase += form->audio_vibrato_rate / SAMPLE_RATE;
        if(form->vibrato_phase >= 1) form->vibrato_phase -= 1;
        vibrato = sinf((float)(form->vibrato_phase * 6.283185307179586)) * form->audio_vibrato_depth;
        instant_frequency = form->audio_frequency * powf(2.0f, vibrato / 12.0f);
        form->phase_a += instant_frequency / SAMPLE_RATE;
        form->phase_b += instant_frequency * form->audio_interval * (1.0f + form->audio_detune) / SAMPLE_RATE;
        form->phase_c += instant_frequency * form->audio_third * (1.0f - form->audio_detune * 0.45f) / SAMPLE_RATE;
        if(form->phase_a >= 1) form->phase_a -= 1;
        if(form->phase_b >= 1) form->phase_b -= 1;
        if(form->phase_c >= 1) form->phase_c -= 1;
        fundamental = sinf((float)(form->phase_a * 6.283185307179586));
        harmonic = sinf((float)(form->phase_b * 6.283185307179586));
        air = sinf((float)(form->phase_c * 6.283185307179586));
        form->noise_state = form->noise_state * 1664525u + 1013904223u;
        noise = ((form->noise_state >> 8) / 8388607.5f - 1.0f) * form->audio_noise;
        signal = fundamental * (0.55f - form->audio_colour * 0.08f);
        signal += harmonic * (0.25f + form->audio_colour * 0.10f);
        signal += air * (0.16f + form->audio_colour * 0.06f + form->audio_strike * 0.24f) + noise * 0.08f;
        signal = tanhf(signal * 0.85f) * form->audio_level;
        {
            float cutoff = clampf(form->audio_frequency * (3.5f + form->audio_colour * 5.0f + form->audio_strike * 4.0f), 450.0f, 4200.0f);
            float g = 1.0f - expf(-6.2831853f * cutoff / SAMPLE_RATE);
            form->low_a += (signal - form->low_a) * g;
            form->low_b += (form->low_a - form->low_b) * g;
            signal = tanhf(form->low_b * VOICE_GAIN * 1.2f) / 1.2f;
        }

        peak = fabsf(signal);
        form->compressor_envelope += (peak - form->compressor_envelope) *
                                     (peak > form->compressor_envelope ? 0.018f : 0.00032f);
        if(form->compressor_envelope > 0.30f) {
            compressed_peak = 0.30f + (form->compressor_envelope - 0.30f) * 0.30f;
            target_compressor_gain = compressed_peak / form->compressor_envelope;
        } else target_compressor_gain = 1.0f;
        form->compressor_gain += (target_compressor_gain - form->compressor_gain) *
                                (target_compressor_gain < form->compressor_gain ? 0.012f : 0.00045f);
        signal *= form->compressor_gain * 1.22f;

        peak = fabsf(signal);
        target_limiter_gain = peak > 0.92f ? 0.92f / peak : 1.0f;
        form->limiter_gain += (target_limiter_gain - form->limiter_gain) *
                             (target_limiter_gain < form->limiter_gain ? 0.32f : 0.00055f);
        signal *= form->limiter_gain;
        {
            float side = form->wide[form->wide_index] * VOICE_WIDTH;
            form->wide[form->wide_index] = signal;
            form->wide_index = (form->wide_index + 1) % WIDTH_DELAY;
            left = (signal + side) * sqrtf((1.0f - form->audio_pan) * 0.5f);
            right = (signal - side) * sqrtf((1.0f + form->audio_pan) * 0.5f);
        }

        {
            float hit_left = 0, hit_right = 0;
            for(int i = 0; i < HITS; i++) {
                Hit *h = &form->hits[i];
                float y;
                if(!h->active) continue;
                y = hit_sample(h) * HIT_GAIN;
                h->pan_now += (h->pan - h->pan_now) * 0.004f;
                hit_left += y * sqrtf((1.0f - h->pan_now) * 0.5f);
                hit_right += y * sqrtf((1.0f + h->pan_now) * 0.5f);
            }
            left += hit_left; right += hit_right;
            if(left != 0 || right != 0) form->quiet = 0;
        }
        left *= OUTPUT_GAIN; right *= OUTPUT_GAIN;

        {
            float in_peak = fmaxf(fabsf(left), fabsf(right)), out_left, out_right;
            float target = in_peak > 0.89f ? 0.89f / in_peak : 1.0f;
            if(target < form->final_gain) form->final_gain = target;
            else form->final_gain += (1.0f - form->final_gain) * 0.00012f;
            form->final_gain_smooth += (form->final_gain - form->final_gain_smooth) *
                                       (form->final_gain < form->final_gain_smooth ? 0.07f : 1.0f);
            out_left = form->delay_left[form->delay_index] * form->final_gain_smooth;
            out_right = form->delay_right[form->delay_index] * form->final_gain_smooth;
            form->delay_left[form->delay_index] = left;
            form->delay_right[form->delay_index] = right;
            form->delay_index = (form->delay_index + 1) % LOOKAHEAD;
            if(form->final_gain_smooth < 0.999f) form->limited_samples++;
            form->total_samples++;
            left = out_left; right = out_right;
            if(fabsf(left) > form->final_peak) form->final_peak = fabsf(left);
            if(fabsf(right) > form->final_peak) form->final_peak = fabsf(right);
        }
        samples[frame * 2] = (int16_t)clampf(left * 32767.0f, -32767, 32767);
        samples[frame * 2 + 1] = (int16_t)clampf(right * 32767.0f, -32767, 32767);
    }
    if(capture) {
        fwrite(stream, 1, bytes, capture);
        capture_bytes += (uint32_t)bytes;
    }
}

static float axis_value(int value)
{
    const int deadzone = 9000;
    if(abs(value) <= deadzone) return 0;
    return (value > 0 ? 1.0f : -1.0f) * (abs(value) - deadzone) / (32767.0f - deadzone);
}

static float trigger_value(Form *form, int trigger, int value)
{
    if(value < -20000) form->trigger_signed[trigger] = 1;
    if(form->trigger_signed[trigger]) return clampf((value + 32768.0f) / 65535.0f, 0, 1);
    if(value < 4000) return 0;
    return clampf((value - 4000.0f) / 28767.0f, 0, 1);
}

static void set_button(Form *form, int button, int pressed)
{
    Controls *c = &form->controls;
    int *pad[4] = {&c->rot_up, &c->rot_down, &c->rot_left, &c->rot_right};
    if(button < 0) return;
    if(button < 32) {
        if(pressed) form->buttons |= 1u << button;
        else form->buttons &= ~(1u << button);
    }
    if((form->buttons & (1u << 12)) && (form->buttons & (1u << 13))) form->running = 0;
    if(button == 0) c->compress = pressed;
    if(button == 1) c->expand = pressed;
    if(button == 2 && pressed && !c->disturb) torsion_pulse(form);
    if(button == 2) c->disturb = pressed;
    if(button == 3) c->colour = pressed;
    if(button == 4) c->damping = pressed;
    if(button == 5) c->resonance = pressed;
    if(button == 6) c->pull = pressed;
    if(button == 7) c->push = pressed;
    if(button >= 8 && button <= 11) {
        if(pressed && !*pad[button - 8]) face_strike(form, button - 8);
        *pad[button - 8] = pressed;
    }
    if(button == 14) { if(pressed && !c->tick) tick_pulse(form); c->tick = pressed; }
    if(button == 15) { if(pressed && !c->knock) knock_pulse(form); c->knock = pressed; }
    if(button == 13 && pressed) release_forces(form);
}

static int controller_action(int button)
{
    switch(button) {
        case SDL_CONTROLLER_BUTTON_A: return 1;
        case SDL_CONTROLLER_BUTTON_B: return 0;
        case SDL_CONTROLLER_BUTTON_X: return 2;
        case SDL_CONTROLLER_BUTTON_Y: return 3;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 4;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return 8;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 9;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 10;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 11;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 14;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 15;
        case SDL_CONTROLLER_BUTTON_BACK: return 12;
        case SDL_CONTROLLER_BUTTON_START: return 13;
        default: return -1;
    }
}

static void set_axis(Form *form, int axis, int raw)
{
    float value = axis_value(raw);
    if(axis == 0) form->controls.rx = value;
    if(axis == 1) form->controls.ry = value;
    if(axis == 2) form->controls.slx = value;
    if(axis == 3) form->controls.sly = value;
    if(axis == 4) form->controls.ltrigger = trigger_value(form, 0, raw);
    if(axis == 5) form->controls.rtrigger = trigger_value(form, 1, raw);
}

static void set_hat(Form *form, Uint8 hat)
{
    set_button(form, 8, (hat & SDL_HAT_UP) != 0);
    set_button(form, 9, (hat & SDL_HAT_DOWN) != 0);
    set_button(form, 10, (hat & SDL_HAT_LEFT) != 0);
    set_button(form, 11, (hat & SDL_HAT_RIGHT) != 0);
}

static SDL_GameController *controller;
static SDL_Joystick *joystick;

static void close_input(void)
{
    if(controller) SDL_GameControllerClose(controller);
    if(joystick) SDL_JoystickClose(joystick);
    controller = NULL;
    joystick = NULL;
}

static void open_input(void)
{
    int count = SDL_NumJoysticks();
    if(controller) return;
    for(int index = 0; index < count && !controller; index++)
        if(SDL_IsGameController(index)) controller = SDL_GameControllerOpen(index);
    if(controller) {
        char *mapping = SDL_GameControllerMapping(controller);
        if(joystick) { SDL_JoystickClose(joystick); joystick = NULL; }
        printf("FORM input: controller %s\n", SDL_GameControllerName(controller));
        if(mapping) { printf("FORM mapping: %s\n", mapping); SDL_free(mapping); }
    } else if(!joystick && count > 0) {
        joystick = SDL_JoystickOpen(0);
        if(joystick) printf("FORM input: raw joystick %s\n", SDL_JoystickName(joystick));
    }
    fflush(stdout);
}

static int active_joystick(SDL_JoystickID which)
{
    return joystick && !controller && SDL_JoystickInstanceID(joystick) == which;
}

static void input_removed(Form *form, SDL_JoystickID which)
{
    int lost = (controller && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == which) ||
               (joystick && SDL_JoystickInstanceID(joystick) == which);
    if(!lost) return;
    close_input();
    release_forces(form);
    form->buttons = 0;
    printf("FORM input: device removed\n");
    open_input();
}

static void handle_event(Form *form, SDL_Event *event)
{
    if(event->type == SDL_QUIT) form->running = 0;
    else if(event->type == SDL_CONTROLLERDEVICEADDED || event->type == SDL_JOYDEVICEADDED) open_input();
    else if(event->type == SDL_CONTROLLERDEVICEREMOVED) input_removed(form, event->cdevice.which);
    else if(event->type == SDL_JOYDEVICEREMOVED) input_removed(form, event->jdevice.which);
    else if(event->type == SDL_CONTROLLERAXISMOTION) {
        if(controller) set_axis(form, event->caxis.axis, event->caxis.value);
    } else if(event->type == SDL_CONTROLLERBUTTONDOWN || event->type == SDL_CONTROLLERBUTTONUP) {
        if(controller) set_button(form, controller_action(event->cbutton.button), event->type == SDL_CONTROLLERBUTTONDOWN);
    } else if(event->type == SDL_JOYAXISMOTION) {
        if(active_joystick(event->jaxis.which)) set_axis(form, event->jaxis.axis, event->jaxis.value);
    } else if(event->type == SDL_JOYHATMOTION) {
        if(active_joystick(event->jhat.which) && event->jhat.hat == 0) set_hat(form, event->jhat.value);
    } else if(event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP) {
        if(active_joystick(event->jbutton.which)) set_button(form, event->jbutton.button, event->type == SDL_JOYBUTTONDOWN);
    } else if(event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        int down = event->type == SDL_KEYDOWN;
        SDL_Keycode key = event->key.keysym.sym;
        Controls *c = &form->controls;
        if(key == SDLK_ESCAPE && down) form->running = 0;
        if(event->key.repeat) return;
        if(key == SDLK_UP) set_button(form, 8, down);
        if(key == SDLK_DOWN) set_button(form, 9, down);
        if(key == SDLK_LEFT) set_button(form, 10, down);
        if(key == SDLK_RIGHT) set_button(form, 11, down);
        if(key == SDLK_n) set_button(form, 14, down);
        if(key == SDLK_m) set_button(form, 15, down);
        if(key == SDLK_a) c->slx = down ? -1 : 0;
        if(key == SDLK_d) c->slx = down ? 1 : 0;
        if(key == SDLK_w) c->sly = down ? -1 : 0;
        if(key == SDLK_s) c->sly = down ? 1 : 0;
        if(key == SDLK_j) c->rx = down ? -1 : 0;
        if(key == SDLK_l) c->rx = down ? 1 : 0;
        if(key == SDLK_i) c->ry = down ? -1 : 0;
        if(key == SDLK_k) c->ry = down ? 1 : 0;
        if(key == SDLK_z) set_button(form, 0, down);
        if(key == SDLK_x) set_button(form, 1, down);
        if(key == SDLK_c) set_button(form, 2, down);
        if(key == SDLK_v) set_button(form, 3, down);
        if(key == SDLK_q) set_button(form, 4, down);
        if(key == SDLK_e) set_button(form, 5, down);
        if(key == SDLK_r) set_button(form, 6, down);
        if(key == SDLK_f) set_button(form, 7, down);
        if(key == SDLK_SPACE && down) release_forces(form);
    }
}

static float closest_to_centre(Form *form)
{
    float closest = 9;
    for(int i = 0; i < VERTICES; i++) {
        Vec3 n = normalise(form->vertices[i].rest), p = sub(form->vertices[i].position, form->shape_centre);
        float reach = (p.x * n.x + p.y * n.y + p.z * n.z) / length3(form->vertices[i].rest);
        if(reach < closest) closest = reach;
    }
    return closest;
}

static float residual(Form *form)
{
    float total = length3(form->centre) + length3(form->centre_velocity) + length3(form->rotation) + length3(form->angular_velocity);
    for(int i = 0; i < VERTICES; i++) total += length3(sub(form->vertices[i].position, form->vertices[i].rest)) + length3(form->vertices[i].velocity);
    return total / (VERTICES + 4);
}

static int play_test(const char *path);
static int self_test(void)
{
    Form form;
    float peak, recovered, single_interval, combined_interval, previous_angle, pivot_error;
    int revolutions = 0;
    int16_t silence[2048], active[2048];
    long long silent_energy = 0, active_energy = 0;
    initialise(&form);
    form.controls.lx = 0.8f; form.controls.ly = -0.55f;
    form.controls.rx = 0.7f; form.controls.ry = -0.4f;
    form.controls.compress = 1; form.controls.resonance = 1; form.controls.pull = 1;
    for(int frame = 0; frame < 360; frame++) simulate(&form, 1.0f / 120.0f);
    torsion_pulse(&form);
    peak = residual(&form);
    release_forces(&form);
    for(int frame = 0; frame < 1440; frame++) simulate(&form, 1.0f / 120.0f);
    recovered = residual(&form);
    memset(silence, 0, sizeof(silence));
    form.audio_frequency = form.target_frequency = 130;
    form.audio_level = form.target_level = 0;
    audio_callback(&form, (Uint8 *)silence, sizeof(silence));
    form.target_level = 0.22f; form.target_colour = 0.6f;
    memset(active, 0, sizeof(active));
    audio_callback(&form, (Uint8 *)active, sizeof(active));
    for(int index = 0; index < 2048; index++) {
        silent_energy += llabs(silence[index]);
        active_energy += llabs(active[index]);
    }
    initialise(&form);
    form.controls.expand = 1;
    simulate(&form, 1.0f / 120.0f);
    single_interval = form.target_interval;
    form.controls.compress = 1;
    simulate(&form, 1.0f / 120.0f);
    combined_interval = form.target_interval;
    initialise(&form);
    form.controls.slx = 1.0f;
    previous_angle = form.rotation.y;
    for(int frame = 0; frame < 900; frame++) {
        simulate(&form, 1.0f / 120.0f);
        if(previous_angle > 2.5f && form.rotation.y < -2.5f) revolutions++;
        previous_angle = form.rotation.y;
    }
    form.controls.slx = 0;
    for(int frame = 0; frame < 1200; frame++) simulate(&form, 1.0f / 120.0f);
    pivot_error = length3(form.shape_centre);
    initialise(&form);
    for(int frame = 0; frame < 120000; frame++) {
        form.controls.lx = sinf(frame * 0.013f);
        form.controls.ly = cosf(frame * 0.017f);
        form.controls.rx = sinf(frame * 0.021f);
        form.controls.ry = cosf(frame * 0.019f);
        form.controls.compress = (frame / 91) & 1;
        form.controls.expand = (frame / 137) & 1;
        form.controls.resonance = (frame / 173) & 1;
        form.controls.pull = (frame / 211) & 1;
        form.controls.push = (frame / 257) & 1;
        if(frame % 997 == 0) torsion_pulse(&form);
        simulate(&form, 1.0f / 120.0f);
    }
    release_forces(&form);
    for(int frame = 0; frame < 1800; frame++) simulate(&form, 1.0f / 120.0f);
    if(play_test(NULL)) { printf("play test failed\n"); return 1; }
    printf("vertices=%d peak=%.4f recovered=%.4f silent=%lld active=%lld voices=%.3f/%.3f turns=%d pivot=%.4f stress=%.4f\n",
           VERTICES, peak, recovered, silent_energy, active_energy, single_interval, combined_interval,
           revolutions, pivot_error, residual(&form));
    return peak > 0.10f && recovered < 0.015f && silent_energy == 0 && active_energy > 1000 &&
           fabsf(single_interval - combined_interval) > 0.01f && revolutions >= 2 &&
           pivot_error < 0.05f && residual(&form) < 0.02f ? 0 : 1;
}

static int folded_cells(Form *form)
{
    int folded = 0;
    for(int face = 0; face < FACES; face++) for(int row = 0; row < GRID - 1; row++) for(int column = 0; column < GRID - 1; column++) {
        int base = face * GRID * GRID + row * GRID + column;
        Vertex *a = &form->vertices[base], *b = &form->vertices[base + 1], *c = &form->vertices[base + GRID];
        Vec3 now = cross3(sub(b->position, a->position), sub(c->position, a->position));
        Vec3 rest = cross3(sub(b->rest, a->rest), sub(c->rest, a->rest));
        if(now.x * rest.x + now.y * rest.y + now.z * rest.z < 0) folded++;
    }
    return folded;
}

static void play_button(Form *form, int button, int frame, int on, int off)
{
    if(frame == on) set_button(form, button, 1);
    if(frame == off) set_button(form, button, 0);
}

static int play_test(const char *path)
{
    static Form form;
    int16_t block[400 * 2];
    float closest = 9, after = -1, rest_residual;
    int frame, release = 120 * 22, end = 120 * 45, folds = 0, x_folds = 0;
    const int beat = 15;
    initialise(&form);
    if(path) begin_capture(path);
    for(frame = 0; frame < end; frame++) {
        int step = frame / beat, at = frame % beat == 0;
        if(frame < 120 * 9) {
            form.controls.slx = 0.5f * sinf(frame * 0.004f);
            form.controls.rx = 0.3f * sinf(frame * 0.0023f);
            if(at && step % 8 == 0) set_button(&form, 14, 1);
            if(at && step % 8 == 1) set_button(&form, 14, 0);
            if(at && step % 4 == 2) set_button(&form, 15, 1);
            if(at && step % 4 == 3) set_button(&form, 15, 0);
            if(at) {
                static const int pattern[16] = {10, -1, 11, -1, 8, -1, 11, 10, -1, 10, 9, -1, 8, -1, 11, -1};
                int b = pattern[step % 16];
                for(int d = 8; d <= 11; d++) set_button(&form, d, 0);
                if(b >= 0) set_button(&form, b, 1);
            }
            if(frame % beat == 5) for(int d = 8; d <= 11; d++) set_button(&form, d, 0);
        } else if(frame < release) {
            int phase = frame % 24;
            form.controls.slx = sinf(frame * 0.013f); form.controls.sly = cosf(frame * 0.011f);
            form.controls.rx = sinf(frame * 0.021f); form.controls.ry = cosf(frame * 0.017f);
            play_button(&form, frame < 120 * 15 ? 0 : 1, frame, 120 * 9, release - 1);
            for(int d = 0; d < 6; d++) {
                int button = d < 4 ? 8 + d : 10 + d;
                play_button(&form, button, phase, d * 4, d * 4 + 2);
            }
            if(frame % 60 == 0) set_button(&form, 2, 1);
            if(frame % 60 == 6) set_button(&form, 2, 0);
        } else if(frame == release) {
            for(int b = 0; b < 16; b++) set_button(&form, b, 0);
            release_forces(&form);
        }
        simulate(&form, 1.0f / 120.0f);
        if(closest_to_centre(&form) < closest) closest = closest_to_centre(&form);
        if(folded_cells(&form) > folds) folds = folded_cells(&form);
        memset(block, 0, sizeof(block));
        audio_callback(&form, (Uint8 *)block, sizeof(block));
        if(frame > release) {
            int loud = 0;
            for(int i = 0; i < 800; i++) if(abs(block[i]) > 1) loud = 1;
            if(loud) after = -1;
            else if(after < 0) after = (frame - release) / 120.0f;
        }
    }
    finish_capture();
    rest_residual = residual(&form);
    {
    float peak_db = 20.0f * log10f(fmaxf(form.final_peak, 1e-6f));
    double limited = 100.0 * form.limited_samples / (double)form.total_samples;
    for(int pass = 0; pass < 2; pass++) {
        Form *f = &form;
        initialise(f);
        if(pass) set_button(f, 0, 1);
        for(frame = 0; frame < 720; frame++) {
            if(frame % 15 == 0) set_button(f, 2, 1);
            if(frame % 15 == 4) set_button(f, 2, 0);
            simulate(f, 1.0f / 120.0f);
            if(folded_cells(f) > x_folds) x_folds = folded_cells(f);
        }
    }
    printf("play: peak %.1f dBFS, limiter working %.1f%% of the time, closest %.2f, silent %.1f s after letting go, residual %.4f, folded cells %d (X hammered %d) of %d\n",
           peak_db, limited, closest, after, rest_residual, folds, x_folds, FACES * (GRID - 1) * (GRID - 1));
    }
    return closest > 0.05f && folds < 12 && x_folds < 12 && after >= 0 && after < 20.0f && rest_residual < 0.02f ? 0 : 1;
}

static int snapshot(const char *path)
{
    Form form;
    SDL_Surface *surface;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    initialise(&form);
    if(SDL_Init(SDL_INIT_VIDEO) < 0) return 2;
    surface = SDL_CreateRGBSurfaceWithFormat(0, WIDTH, HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
    renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    if(!renderer) return 3;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
    if(!texture) return 4;
    form.controls.lx = 0.7f; form.controls.ly = -0.4f; form.controls.rx = 0.65f;
    form.controls.expand = 1; form.controls.resonance = 1;
    for(int frame = 0; frame < 180; frame++) simulate(&form, 1.0f / 120.0f);
    render(&form, renderer, texture);
    SDL_SaveBMP(surface, path);
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
    return 0;
}

static int benchmark(void)
{
    Form form;
    SDL_Surface *surface;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint64_t start, end, frequency;
    initialise(&form);
    if(SDL_Init(SDL_INIT_VIDEO) < 0) return 2;
    surface = SDL_CreateRGBSurfaceWithFormat(0, WIDTH, HEIGHT, 32, SDL_PIXELFORMAT_ARGB8888);
    renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT) : NULL;
    if(!texture) return 3;
    form.controls.lx = 0.65f; form.controls.rx = 0.55f; form.controls.expand = 1; form.controls.resonance = 1;
    frequency = SDL_GetPerformanceFrequency(); start = SDL_GetPerformanceCounter();
    for(int frame = 0; frame < 240; frame++) {
        simulate(&form, 1.0f / 120.0f); simulate(&form, 1.0f / 120.0f);
        render(&form, renderer, texture);
    }
    end = SDL_GetPerformanceCounter();
    printf("frames=240 milliseconds=%.2f fps=%.2f\n", (end - start) * 1000.0 / frequency, 240.0 * frequency / (end - start));
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
    return 0;
}

static void fault(int signal_number)
{
    const char *name = signal_number == SIGSEGV ? "memory fault" : signal_number == SIGBUS ? "bus error" :
                       signal_number == SIGFPE ? "arithmetic fault" : signal_number == SIGILL ? "illegal instruction" :
                       signal_number == SIGABRT ? "abort" : "signal";
    char line[96];
    int length = snprintf(line, sizeof line, "FORM stopped: %s (signal %d)\n", name, signal_number);
    if(length > 0) { ssize_t ignored = write(STDERR_FILENO, line, (size_t)length); (void)ignored; }
#if defined(__GLIBC__)
    {
        void *frames[32];
        int count = backtrace(frames, 32);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }
#endif
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void watch_for_faults(void)
{
    signal(SIGSEGV, fault); signal(SIGBUS, fault); signal(SIGFPE, fault);
    signal(SIGILL, fault); signal(SIGABRT, fault);
}

static long read_number(const char *path)
{
    long value = -1;
    FILE *file = fopen(path, "r");
    if(file) { if(fscanf(file, "%ld", &value) != 1) value = -1; fclose(file); }
    return value;
}

static void heartbeat(const Form *form, double seconds, double slowest_frame, double average_frame)
{
    long temperature = read_number("/sys/class/thermal/thermal_zone0/temp");
    long clock = read_number("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    printf("FORM %.0f s: chip %.1f C, clock %ld MHz, frames %.1f ms (slowest %.1f), held buttons %05x\n",
           seconds, temperature > 1000 ? temperature / 1000.0 : (double)temperature,
           clock > 0 ? clock / 1000 : clock, average_frame, slowest_frame, form->buttons);
    fflush(stdout);
}

static int at_rest(const Form *form)
{
    const Controls *c = &form->controls;
    if(form->awake < 2.5f || form->buttons || form->audio_level != 0 || form->target_level != 0) return 0;
    if(c->slx != 0 || c->sly != 0 || c->rx != 0 || c->ry != 0 || c->ltrigger != 0 || c->rtrigger != 0) return 0;
    if(length3(form->seen_angular_velocity) > 0.002f || length3(form->seen_centre_velocity) > 0.002f || length3(form->seen_centre) > 0.002f || form->motion > 0.0004f || form->deformation > 0.0004f || form->strike > 0.001f || form->line_flash > 0.003f) return 0;
    for(int face = 0; face < FACES; face++) if(form->face_light[face] > 0) return 0;
    return 1;
}

static int report(const char *what, int code)
{
    fprintf(stderr, "FORM: %s: %s\n", what, SDL_GetError());
    return code;
}

int main(int argc, char **argv)
{
    Form form;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_RendererInfo info;
    SDL_DisplayMode mode;
    SDL_AudioDeviceID audio = 0;
    SDL_AudioSpec wanted;
    uint64_t previous, accumulator = 0;
    uint64_t frequency, frame_period, next_frame;
    int vsync = 0, capped, fast_frames = 0, refresh = 60;
    setvbuf(stdout, NULL, _IOLBF, 0);
    watch_for_faults();
    if(argc > 1 && strcmp(argv[1], "--self-test") == 0) return self_test();
    if(argc > 2 && strcmp(argv[1], "--play-test") == 0) return play_test(argv[2]);
    if(argc > 2 && strcmp(argv[1], "--snapshot") == 0) return snapshot(argv[2]);
    if(argc > 1 && strcmp(argv[1], "--benchmark") == 0) return benchmark();
    initialise(&form);
    form.awake = -BADGE_TIME;
    roll_character(getenv("FORM_SEED") ? (uint32_t)strtoul(getenv("FORM_SEED"), NULL, 10) : (uint32_t)time(NULL) ^ (uint32_t)SDL_GetPerformanceCounter());
    form.seen_rotation = character.rest;
    printf("FORM seed %u: faces %.3f %.3f %.3f %.3f %.3f %.3f, knock %.2f %.2f %.2f, tick %.2f %.2f, light %+.2f, size %.2f\n", character.seed,
           character.note[3], character.note[0], character.note[4], character.note[1], character.note[5], character.note[2],
           character.knock_bend, character.knock_ring, character.knock_warmth, character.tick_band, character.tick_spread, character.warmth, character.size);
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) return report("could not start video and input", 2);
#ifdef __linux__
    window = SDL_CreateWindow("FORM", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              WIDTH, HEIGHT, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
#else
    window = SDL_CreateWindow("FORM", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
#endif
    if(!window) return report("could not open the window", 3);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if(!renderer) {
        report("no accelerated renderer, trying software", 0);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if(!renderer) return report("could not create a renderer", 4);
    SDL_RenderSetLogicalSize(renderer, WIDTH, HEIGHT);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
    if(!texture) return report("could not create the picture texture", 5);
    render_reset();
    if(SDL_GetRendererInfo(renderer, &info) == 0) {
        vsync = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
        printf("FORM video: %s renderer, vsync %s\n", info.name, vsync ? "on" : "off");
    }
    if(SDL_GetWindowDisplayMode(window, &mode) == 0 && mode.refresh_rate >= 30 && mode.refresh_rate <= 240)
        refresh = mode.refresh_rate;
    open_input();

    if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) report("no sound, continuing without it", 0);
    else {
        SDL_zero(wanted);
        wanted.freq = SAMPLE_RATE; wanted.format = AUDIO_S16SYS; wanted.channels = 2; wanted.samples = 512;
        wanted.callback = audio_callback; wanted.userdata = &form;
        audio = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
        if(audio) SDL_PauseAudioDevice(audio, 0);
        else report("could not open the sound device, continuing without sound", 0);
    }

    frequency = SDL_GetPerformanceFrequency();
    frame_period = frequency / refresh;
    capped = !vsync;
    if(capped) printf("FORM video: pacing frames at %d fps\n", refresh);
    previous = next_frame = SDL_GetPerformanceCounter();
    {
    uint64_t started = previous, beat = previous;
    double slowest = 0, total = 0;
    int counted = 0, woken = 0;
    int resting = 0;
    while(form.running) {
        SDL_Event event;
        uint64_t now;
        if(resting) {
            if(SDL_WaitEventTimeout(&event, 250)) {
                handle_event(&form, &event);
                while(SDL_PollEvent(&event)) handle_event(&form, &event);
            }
            previous = SDL_GetPerformanceCounter();
            accumulator = 0;
            if(at_rest(&form)) continue;
            resting = 0;
        }
        now = SDL_GetPerformanceCounter();
        uint64_t elapsed = now - previous, finished;
        previous = now;
        if(elapsed > frequency / 10) elapsed = frequency / 10;
        accumulator += elapsed;
        while(SDL_PollEvent(&event)) handle_event(&form, &event);
        if(!woken && form.awake >= 0 && form.target_frequency > 0) { request_hit(&form, HIT_STRIKE, 3, 0.55f, 0); woken = 1; }
        while(accumulator >= frequency / 120) {
            simulate(&form, 1.0f / 120.0f);
            accumulator -= frequency / 120;
        }
        render(&form, renderer, texture);
        resting = at_rest(&form);
        finished = SDL_GetPerformanceCounter();
        {
            double frame_ms = (finished - now) * 1000.0 / frequency;
            if(frame_ms > slowest) slowest = frame_ms;
            total += frame_ms; counted++;
            if(finished - beat > frequency * 5 && counted) {
                heartbeat(&form, (finished - started) / (double)frequency, slowest, total / counted);
                beat = finished; slowest = 0; total = 0; counted = 0;
            }
        }
        if(!capped) {
            if(finished - now < frame_period / 2) {
                if(++fast_frames > 30) {
                    capped = 1;
                    next_frame = finished;
                    printf("FORM video: vsync not holding, pacing frames at %d fps\n", refresh);
                }
            } else fast_frames = 0;
        }
        if(capped) {
            next_frame += frame_period;
            if(finished > next_frame + frame_period) next_frame = finished;
            else if(next_frame > finished) SDL_Delay((Uint32)((next_frame - finished) * 1000 / frequency));
        }
    }
    }
    printf("FORM finished normally\n");
    if(audio) SDL_CloseAudioDevice(audio);
    close_input();
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}

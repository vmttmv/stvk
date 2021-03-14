#version 450
#extension GL_ARB_separate_shader_objects : enable

#ifdef VERTEX_SHADER
struct Rect {
        uint pos;
        uint uv[2];
        uint fg;
        uint bg;
};

const vec2[6] lut = vec2[6](
        vec2(0.0, 1.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 0.0),
        vec2(0.0, 1.0)
);

layout(location = 0) out vec2 fsUV;
layout(location = 1) out vec4 fsFG;
layout(location = 2) out vec4 fsBG;

layout(push_constant) uniform u_constants {
        vec2 view;
        vec2 texSize;
} pc;

layout(set = 0, binding = 0) readonly buffer b_vertices {
        Rect data[];
};

vec4 unpack_rgba(uint c)
{
        float a = ((c >> 24) & 0xff) / 255.0;
        float b = ((c >> 16) & 0xff) / 255.0;
        float g = ((c >> 8) & 0xff) / 255.0;
        float r = (c & 0xff) / 255.0;
        return vec4(r, g, b, a);
}

void main()
{
        Rect r = data[gl_VertexIndex / 6];
        float u = float(r.uv[0] & 0xffff);
        float v = float(r.uv[0] >> 16);
        float w = float(r.uv[1] & 0xffff);
        float h = float(r.uv[1] >> 16);

        vec2 base = lut[gl_VertexIndex % 6];
        vec2 p = vec2(float(r.pos & 0xffff), float(r.pos >> 16)) + vec2(w, h) * base;

        gl_Position = vec4(2.0*p.x/pc.view.x-1.0, 2.0*p.y/pc.view.y-1.0, 0.0, 1.0);

        fsFG = unpack_rgba(r.fg);
        fsBG = unpack_rgba(r.bg);

        if (u > pc.texSize.x) {
                fsUV = vec2(0.0);
        } else {
                fsUV = vec2(u + w*base.x, v + h*base.y) / pc.texSize;
        }
}
#endif

#ifdef FRAGMENT_SHADER
layout(location = 0) in vec2 fsUV;
layout(location = 1) in vec4 fsFG;
layout(location = 2) in vec4 fsBG;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D u_sampler;

void main()
{
        float t = texture(u_sampler, fsUV).r;
        fragColor = fsFG*t + fsBG*(1.0-t);
}
#endif

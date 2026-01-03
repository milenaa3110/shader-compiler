
test if
fn vec4 shade(vec2 uv) {
float d = length(uv - vec2(0.5, 0.5));

    if (d < 0.3) {
        float t = d / 0.3;
        return vec4(1.0 - t, 0.3, t, 1.0);
    } else {
        float t = (d - 0.3) / 0.7;
        return vec4(0.1, t, 1.0 - t, 1.0);
    }
    return vec4(0.0, 0.0, 0.0, 1.0);
}

test loops

#1

fn vec4 shade(vec2 uv) {
float d = length(uv - vec2(0.5, 0.5));
vec4 color = vec4(0.0, 0.0, 0.0, 1.0);

    for (int i = 0; i < 5; i = i + 1) {
        float ring = float(i) * 0.1;
        float width = 0.05;
        
        if (d > ring && d < ring + width) {
            float t = float(i) / 5.0;
            color = vec4(t, 1.0 - t, 0.5, 1.0);
        }
    }
    
    return color;
}

#2

fn vec4 shade(vec2 uv) {
vec2 center = vec2(0.5, 0.5);
float d = length(uv - center);

    float intensity = 0.0;
    float ripple = 0.1;
    
    while (ripple < 1.0) {
        float wave = sin((d - ripple) * 50.0);
        intensity = intensity + wave * (1.0 - ripple);
        ripple = ripple + 0.2;
    }
    
    intensity = intensity * 0.5 + 0.5;
    return vec4(intensity, intensity * 0.8, 1.0, 1.0);
}

#3

fn vec4 shade(vec2 uv) {
vec3 finalColor = vec3(0.0, 0.0, 0.0);

    for (int i = 0; i < 8; i = i + 1) {
        float band = float(i) * 0.125;
        
        if (uv.x > band && uv.x < band + 0.125) {
            float t = float(i) / 8.0;
            finalColor = vec3(t, 1.0 - t, uv.y);
        }
    }
    
    return vec4(finalColor, 1.0);
}

#4

fn vec4 shade(vec2 uv) {
vec3 color = vec3(0.0, 0.0, 0.0);
int particle = 0;

    while (particle < 6) {
        float angle = float(particle) * 1.047;  # ~60 degrees
        float radius = 0.3;
        
        vec2 pos = vec2(
            0.5 + radius * sin(angle),
            0.5 + radius * cos(angle)
        );
        
        float dist = length(uv - pos);
        
        if (dist < 0.08) {
            float t = float(particle) / 6.0;
            color = vec3(1.0 - t, t, 0.5);
        }
        
        particle = particle + 1;
    }
    
    return vec4(color, 1.0);
}

#5

fn vec4 shade(vec2 uv) {
vec3 color = vec3(0.0, 0.0, 0.0);
int cells = 8;

    for (int x = 0; x < cells; x = x + 1) {
        for (int y = 0; y < cells; y = y + 1) {
            float x0 = float(x) / float(cells);
            float y0 = float(y) / float(cells);
            float x1 = float(x + 1) / float(cells);
            float y1 = float(y + 1) / float(cells);
            
            if (uv.x > x0 && uv.x < x1 && uv.y > y0 && uv.y < y1) {
                if (mod(float(x + y), 2.0) < 0.5) {
                    color = vec3(1.0, 1.0, 1.0);
                } else {
                    float t = (float(x) + float(y)) / float(cells * 2);
                    color = vec3(t, 0.3, 1.0 - t);
                }
            }
        }
    }
    
    return vec4(color, 1.0);
}

Struct test

struct Light {
    float intensity;
    vec3 color;
};

Light L = Light();
L.intensity = 2.0;
L.color = vec3(1.0, 0.5, 0.0);
float x = L.intensity;
vec3  c = L.color;

// Procedural noise test

fn float hash(float n) {
return fract(sin(n) * 43758.5453);
}

fn float noise(vec2 p) {
vec2 i = floor(p);
vec2 f = fract(p);

    float a = hash(i.x + i.y * 57.0);
    float b = hash(i.x + 1.0 + i.y * 57.0);
    float c = hash(i.x + (i.y + 1.0) * 57.0);
    float d = hash(i.x + 1.0 + (i.y + 1.0) * 57.0);

    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

fn vec4 shade(vec2 uv) {
float n = noise(uv * 10.0);
return vec4(n, n, n, 1.0);
}


// Moonlight test

uniform vec3 cameraPos;
uniform vec3 lightPos;
uniform float uTime;

fn float sdSphere(vec3 p, float r) {
return length(p) - r;
}

fn float scene(vec3 p) {
vec3 offset = vec3(sin(uTime), 0.0, cos(uTime));
return sdSphere(p - offset, 1.0);
}

fn vec3 getNormal(vec3 p) {
float eps = 0.001;
float d = scene(p);
return normalize(vec3(
scene(vec3(p.x + eps, p.y, p.z)) - d,
scene(vec3(p.x, p.y + eps, p.z)) - d,
scene(vec3(p.x, p.y, p.z + eps)) - d
));
}

fn vec4 shade(vec2 uv) {
vec3 ro = cameraPos;
vec3 rd = normalize(vec3(uv.x - 0.5, uv.y - 0.5, 1.0));

    float t = 0.0;
    for (int i = 0; i < 64; i = i + 1) {
        vec3 p = ro + rd * t;
        float d = scene(p);
        if (d < 0.001) {
            vec3 n = getNormal(p);
            vec3 l = normalize(lightPos - p);
            float diff = max(dot(n, l), 0.0);
            return vec4(diff, diff, diff, 1.0);
        }
        t = t + d;
        if (t > 100.0) break;
    }

    return vec4(0.0, 0.0, 0.0, 1.0);
}

cameraPos = {0.0f, 0.0f, -5.0f};
lightPos  = {2.0f, 3.0f, 1.0f};
uTime = 1.0f;

struct vec3 {
float x;
float y;
float z;
};
extern "C" {
void shade_wrapper(float u, float v, float *out);
extern vec3 cameraPos;
extern vec3 lightPos;
extern float uTime;
}

// mandelbrot test

uniform int maxIterations;
uniform vec2 center;
uniform float zoom;

fn vec4 shade(vec2 uv) {
vec2 c = (uv - vec2(0.5, 0.5)) * zoom + center;
vec2 z = vec2(0.0, 0.0);

    int iter = 0;
    for (int i = 0; i < 256; i = i + 1) {
        if (i >= maxIterations) break;
        if (dot(z, z) > 4.0) break;

        float xtemp = z.x * z.x - z.y * z.y + c.x;
        z.y = 2.0 * z.x * z.y + c.y;
        z.x = xtemp;
        iter = iter + 1;
    }

    float t = float(iter) / float(maxIterations);
    vec3 color = vec3(t, t * t, t * t * t);
    return vec4(color, 1.0);
}

extern "C" {
void shade_wrapper(float u, float v, float *out);
extern  int maxIterations;
extern vec2 center;
extern float zoom;
}

int main() {
maxIterations = 300;
center = {-1.0f, 0.0f};
zoom = 0.5f;

complex shape

struct Ray {
vec3 origin;
vec3 direction;
};

struct Hit {
bool hit;
float t;
vec3 point;
vec3 normal;
};

fn Hit raycast(Ray r, vec3 sphereCenter, float radius) {
Hit h;
h.hit = false;

    vec3 oc = r.origin - sphereCenter;
    float a = dot(r.direction, r.direction);
    float b = 2.0 * dot(oc, r.direction);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = b * b - 4.0 * a * c;

    if (discriminant > 0.0) {
        float t = (-b - sqrt(discriminant)) / (2.0 * a);
        if (t > 0.0) {
            h.hit = true;
            h.t = t;
            h.point = r.origin + r.direction * t;
            h.normal = normalize(h.point - sphereCenter);
        }
    }

    return h;
}

fn vec4 shade(vec2 uv) {
Ray r;
r.origin = vec3(0.0, 0.0, -5.0);
r.direction = normalize(vec3(uv.x - 0.5, uv.y - 0.5, 1.0));

    Hit h = raycast(r, vec3(0.0, 0.0, 0.0), 1.0);

    if (h.hit) {
        float lighting = max(dot(h.normal, normalize(vec3(1.0, 1.0, -1.0))), 0.0);
        return vec4(lighting, lighting, lighting, 1.0);
    }

    return vec4(0.2, 0.2, 0.3, 1.0);
}

complete test

struct Light {
    vec3 position;
    vec3 color;
    float intensity;
};

struct Material {
    vec3 ambient;
    vec3 diffuse;
    float shininess;
};

uniform vec3 lightPos;
uniform vec3 lightColor;
uniform float time;
uniform vec3 lights[4];

fn float add(float a, float b) {
    return a + b;
}

fn float arithmetic_test(float x, float y) {
    float sum = x + y;
    float diff = x - y;
    float prod = x * y;
    float quot = x / y;
    return sum + diff + prod + quot;
}

fn bool comparison_test(float a, float b) {
    bool lt = a < b;
    bool le = a <= b;
    bool gt = a > b;
    bool ge = a >= b;
    bool eq = a == b;
    bool ne = a != b;
    return lt || le || gt || ge || eq || ne;
}

fn bool logical_test(bool a, bool b) {
    bool and_result = a && b;
    bool or_result = a || b;
    bool not_result = !a;
    return and_result || or_result || not_result;
}

fn float unary_test(float x) {
    float neg = -x;
    return neg;
}

fn vec3 vector_test(vec3 v1, vec3 v2) {
    vec3 sum = v1 + v2;
    vec3 diff = v1 - v2;
    vec3 prod = v1 * v2;
    
    
    float x = v1.x;
    float y = v1.y;
    float z = v1.z;
    
    vec2 xy = v1.xy;
    vec3 zyx = v1.zyx;
    
    return sum + diff + prod;
}

fn vec3 swizzle_assignment_test() {
    vec3 v = vec3(1.0, 2.0, 3.0);
    v.x = 5.0;
    v.xy = vec2(10.0, 20.0);
    v.xyz = vec3(1.0, 1.0, 1.0);
    return v;
}

fn vec3 struct_test() {
    Light light;
    light.position = vec3(1.0, 2.0, 3.0);
    light.color = vec3(1.0, 1.0, 1.0);
    light.intensity = 0.8;
    
    vec3 pos = light.position;
    return pos;
}

fn float nested_member_test() {
    Light light;
    light.position = vec3(5.0, 10.0, 15.0);
    
    float x = light.position.x;
    return x;
}


fn float if_test(float x) {
    if (x > 0.0) {
        return 1.0;
    }
    return -1.0;
}


fn float if_else_test(float x) {
    if (x > 0.0) {
        return 1.0;
    } else {
        return -1.0;
    }
}


fn float nested_if_test(float x, float y) {
    if (x > 0.0) {
        if (y > 0.0) {
            return 1.0;
        } else {
            return 2.0;
        }
    } else {
        return 3.0;
    }
}


fn float while_test(float n) {
    float sum = 0.0;
    float i = 0.0;
    while (i < n) {
        sum = sum + i;
        i = i + 1.0;
    }
    return sum;
}


fn float for_test(float n) {
    float sum = 0.0;
    for (float i = 0.0; i < n; i = i + 1.0) {
        sum = sum + i;
    }
    return sum;
}


fn float for_break_test(float limit) {
    float sum = 0.0;
    for (float i = 0.0; i < 100.0; i = i + 1.0) {
        sum = sum + i;
        if (sum > limit) {
            break;
        }
    }
    return sum;
}


fn float while_break_test(float limit) {
    float sum = 0.0;
    float i = 0.0;
    while (i < 100.0) {
        sum = sum + i;
        if (sum > limit) {
            break;
        }
        i = i + 1.0;
    }
    return sum;
}


fn float array_test() {
    float arr[3] = { 1.0, 2.0, 3.0 };
    float sum = arr[0] + arr[1] + arr[2];
    return sum;
}


fn float variable_test() {
    float a = 1.0;
    float b = 2.0;
    float c;
    c = a + b;
    return c;
}


fn bool boolean_test() {
    bool t = true;
    bool f = false;
    return t && !f;
}


fn float cast_test() {
    int i = 5;
    float f = float(i);
    return f;
}


fn vec4 constructor_test() {
    vec2 v2 = vec2(1.0, 2.0);
    vec3 v3 = vec3(1.0, 2.0, 3.0);
    vec4 v4 = vec4(1.0, 2.0, 3.0, 4.0);
    return v4;
}


fn float precedence_test(float a, float b, float c) {
    float result = a + b * c - a / b;
    return result;
}


fn vec3 uniform_array_test(int index) {
    vec3 light = lights[index];
    return light;
}


fn float multiple_returns(float x) {
    if (x < 0.0) {
        return -1.0;
    }
    if (x == 0.0) {
        return 0.0;
    }
    return 1.0;
}


fn void void_function() {
    float x = 1.0;
    float y = 2.0;
    float z = x + y;
    return;
}


fn float complex_control_flow(float x, float y) {
    float result = 0.0;
    
    for (float i = 0.0; i < 5.0; i = i + 1.0) {
        if (i < x) {
            result = result + i;
        } else {
            if (i < y) {
                result = result + i * 2.0;
            } else {
                break;
            }
        }
    }
    
    return result;
}


fn float all_vector_types() {
    vec2 v2 = vec2(1.0, 2.0);
    vec3 v3 = vec3(1.0, 2.0, 3.0);
    vec4 v4 = vec4(1.0, 2.0, 3.0, 4.0);
    
    float sum = v2.x + v3.y + v4.z;
    return sum;
}


fn vec4 shade(vec2 uv) {
    
    float x = uv.x + 0.5;
    float y = uv.y * 2.0;
    
    
    vec3 color;
    if (x > 1.0) {
        color = vec3(1.0, 0.0, 0.0);
    } else {
        color = vec3(0.0, 1.0, 0.0);
    }
    
    
    float brightness = 0.0;
    for (int i = 0; i < 8; i = i + 1) {
        float band = float(i) * 0.125;
        
        if (uv.x > band && uv.x < band + 0.125) {
            float t = float(i) / 8.0;
            brightness = t;
            color = vec3(t, 1.0 - t, uv.y);
        }
    }
    
    
    float counter = 0.0;
    while (counter < brightness) {
        counter = counter + 0.1;
    }
    
    
    bool inRange = uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0;
    if (inRange) {
        color = color * 1.2;
    }
    
    
    float negY = -uv.y;
    if (negY < -0.5) {
        color = color + vec3(0.1, 0.1, 0.1);
    }
    
    return vec4(color, 1.0);
}

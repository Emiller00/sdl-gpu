#include "SDL_gpu.h"
#include "SDL_platform.h"
#include <string.h>
#include <strings.h>

#define MAX_ACTIVE_RENDERERS 20
#define MAX_REGISTERED_RENDERERS 10

typedef struct RendererRegistration
{
	GPU_RendererID id;
	GPU_Renderer* (*createFn)(GPU_RendererID request);
	void (*freeFn)(GPU_Renderer*);
} RendererRegistration;

static Uint8 initialized = 0;

static GPU_Renderer* rendererMap[MAX_ACTIVE_RENDERERS];
static RendererRegistration rendererRegister[MAX_REGISTERED_RENDERERS];



static GPU_RendererID makeRendererID(GPU_RendererEnum id, int major_version, int minor_version, int index)
{
    GPU_RendererID r = {id, major_version, minor_version, 0, index};
    return r;
}


void GPU_InitRendererRegister(void);

int GPU_GetNumActiveRenderers(void)
{
	GPU_InitRendererRegister();

	int count = 0;
	int i;
	for(i = 0; i < MAX_ACTIVE_RENDERERS; i++)
	{
		if(rendererMap[i] != NULL)
			count++;
	}
	return count;
}

void GPU_GetActiveRendererList(GPU_RendererID* renderers_array)
{
	GPU_InitRendererRegister();

	int count = 0;
	
	int i;
	for(i = 0; i < MAX_ACTIVE_RENDERERS; i++)
	{
		if(rendererMap[i] != NULL)
		{
			renderers_array[count] = rendererMap[i]->id;
			count++;
		}
	}
}


int GPU_GetNumRegisteredRenderers(void)
{
	GPU_InitRendererRegister();

	int count = 0;
	int i;
	for(i = 0; i < MAX_REGISTERED_RENDERERS; i++)
	{
		if(rendererRegister[i].id.id != GPU_RENDERER_UNKNOWN)
			count++;
	}
	return count;
}

void GPU_GetRegisteredRendererList(GPU_RendererID* renderers_array)
{
	GPU_InitRendererRegister();

	int count = 0;
	
	int i;
	for(i = 0; i < MAX_REGISTERED_RENDERERS; i++)
	{
		if(rendererRegister[i].id.id != GPU_RENDERER_UNKNOWN)
		{
			renderers_array[count] = rendererRegister[i].id;
			count++;
		}
	}
}


GPU_RendererID GPU_GetRendererID(unsigned int index)
{
	if(index >= MAX_REGISTERED_RENDERERS)
		return makeRendererID(GPU_RENDERER_UNKNOWN, 0, 0, -1);
	
	return rendererRegister[index].id;
}

GPU_Renderer* GPU_CreateRenderer_OpenGL_1(GPU_RendererID request);
void GPU_FreeRenderer_OpenGL_1(GPU_Renderer* renderer);
GPU_Renderer* GPU_CreateRenderer_OpenGL_2(GPU_RendererID request);
void GPU_FreeRenderer_OpenGL_2(GPU_Renderer* renderer);
GPU_Renderer* GPU_CreateRenderer_OpenGL_3(GPU_RendererID request);
void GPU_FreeRenderer_OpenGL_3(GPU_Renderer* renderer);
GPU_Renderer* GPU_CreateRenderer_GLES_1(GPU_RendererID request);
void GPU_FreeRenderer_GLES_1(GPU_Renderer* renderer);
GPU_Renderer* GPU_CreateRenderer_GLES_2(GPU_RendererID request);
void GPU_FreeRenderer_GLES_2(GPU_Renderer* renderer);

void GPU_RegisterRenderers()
{
	int i = 0;
	
	if(i >= MAX_REGISTERED_RENDERERS)
		return;
	
	#ifndef SDL_GPU_DISABLE_OPENGL
        #ifndef SDL_GPU_DISABLE_OPENGL_1
        rendererRegister[i].id = makeRendererID(GPU_RENDERER_OPENGL_1, 1, 1, i);
        rendererRegister[i].createFn = &GPU_CreateRenderer_OpenGL_1;
        rendererRegister[i].freeFn = &GPU_FreeRenderer_OpenGL_1;
        
        i++;
        if(i >= MAX_REGISTERED_RENDERERS)
            return;
        #endif
	
        #ifndef SDL_GPU_DISABLE_OPENGL_2
        rendererRegister[i].id = makeRendererID(GPU_RENDERER_OPENGL_2, 2, 0, i);
        rendererRegister[i].createFn = &GPU_CreateRenderer_OpenGL_2;
        rendererRegister[i].freeFn = &GPU_FreeRenderer_OpenGL_2;
        
        i++;
        if(i >= MAX_REGISTERED_RENDERERS)
            return;
        #endif
	
        #ifndef SDL_GPU_DISABLE_OPENGL_3
        rendererRegister[i].id = makeRendererID(GPU_RENDERER_OPENGL_3, 3, 0, i);
        rendererRegister[i].createFn = &GPU_CreateRenderer_OpenGL_3;
        rendererRegister[i].freeFn = &GPU_FreeRenderer_OpenGL_3;
        
        i++;
        if(i >= MAX_REGISTERED_RENDERERS)
            return;
        #endif
    #endif
	
	#ifndef SDL_GPU_DISABLE_GLES
        #ifndef SDL_GPU_DISABLE_GLES_1
        rendererRegister[i].id = makeRendererID(GPU_RENDERER_GLES_1, 1, 1, i);
        rendererRegister[i].createFn = &GPU_CreateRenderer_GLES_1;
        rendererRegister[i].freeFn = &GPU_FreeRenderer_GLES_1;
        
        i++;
        if(i >= MAX_REGISTERED_RENDERERS)
            return;
        #endif
        #ifndef SDL_GPU_DISABLE_GLES_2
        rendererRegister[i].id = makeRendererID(GPU_RENDERER_GLES_2, 2, 0, i);
        rendererRegister[i].createFn = &GPU_CreateRenderer_GLES_2;
        rendererRegister[i].freeFn = &GPU_FreeRenderer_GLES_2;
        
        i++;
        if(i >= MAX_REGISTERED_RENDERERS)
            return;
        #endif
    #endif
	
}


static int renderer_order_size = 0;
static GPU_RendererID renderer_order[GPU_RENDERER_ORDER_MAX];

void GPU_InitRendererRegister(void)
{
	if(initialized)
		return;
	
	int i;
	for(i = 0; i < MAX_REGISTERED_RENDERERS; i++)
	{
		rendererRegister[i].id.id = GPU_RENDERER_UNKNOWN;
		rendererRegister[i].id.index = i;
		rendererRegister[i].createFn = NULL;
		rendererRegister[i].freeFn = NULL;
	}
	for(i = 0; i < MAX_ACTIVE_RENDERERS; i++)
	{
		rendererMap[i] = NULL;
	}
	
	GPU_GetDefaultRendererOrder(&renderer_order_size, renderer_order);
	
	initialized = 1;
	
	GPU_RegisterRenderers();
}


void GPU_GetRendererOrder(int* order_size, GPU_RendererID* order)
{
    if(order_size != NULL)
        *order_size = renderer_order_size;
    
    if(order != NULL && renderer_order_size > 0)
        memcpy(order, renderer_order, renderer_order_size*sizeof(GPU_RendererID));
}



void GPU_GetDefaultRendererOrder(int* order_size, GPU_RendererID* order)
{
    int count = 0;
    GPU_RendererID default_order[GPU_RENDERER_ORDER_MAX];
    
    #if defined(__ANDROID__) || defined(__IPHONEOS__)
        default_order[count++] = GPU_MakeRendererID(GPU_RENDERER_GLES_2, 2, 0, 0);
        default_order[count++] = GPU_MakeRendererID(GPU_RENDERER_GLES_1, 1, 1, 0);
    #else
        default_order[count++] = GPU_MakeRendererID(GPU_RENDERER_OPENGL_3, 3, 0, 0);
        default_order[count++] = GPU_MakeRendererID(GPU_RENDERER_OPENGL_2, 2, 0, 0);
        default_order[count++] = GPU_MakeRendererID(GPU_RENDERER_OPENGL_1, 1, 1, 0);
    #endif
    
    if(order_size != NULL)
        *order_size = count;
    
    if(order != NULL && count > 0)
        memcpy(order, default_order, count*sizeof(GPU_RendererID));
}

const char* GPU_GetRendererEnumString(GPU_RendererEnum id)
{
    if(id == GPU_RENDERER_OPENGL_1)
        return "OpenGL 1.x";
    if(id == GPU_RENDERER_OPENGL_2)
        return "OpenGL 2.x";
    if(id == GPU_RENDERER_OPENGL_3)
        return "OpenGL 3.x";
    if(id == GPU_RENDERER_OPENGL_4)
        return "OpenGL 4.x";
    if(id == GPU_RENDERER_GLES_1)
        return "OpenGLES 1.x";
    if(id == GPU_RENDERER_GLES_2)
        return "OpenGLES 2.x";
    if(id == GPU_RENDERER_GLES_3)
        return "OpenGLES 3.x";
    if(id == GPU_RENDERER_D3D9)
        return "Direct3D 9";
    if(id == GPU_RENDERER_D3D10)
        return "Direct3D 10";
    if(id == GPU_RENDERER_D3D11)
        return "Direct3D 11";
    
    return "Unknown";
}


GPU_Renderer* GPU_CreateRenderer(GPU_RendererID id)
{
	GPU_Renderer* result = NULL;
	int i;
	for(i = 0; i < MAX_REGISTERED_RENDERERS; i++)
	{
		if(rendererRegister[i].id.id == GPU_RENDERER_UNKNOWN)
			continue;
		
		if(id.id == rendererRegister[i].id.id)
		{
			if(rendererRegister[i].createFn != NULL)
            {
				result = rendererRegister[i].createFn(id);
            }
			break;
		}
	}
	
	if(result == NULL)
		GPU_LogError("Could not create renderer: \"%s %d.%d\" was not found in the renderer registry.\n", GPU_GetRendererEnumString(id.id), id.major_version, id.minor_version);
	return result;
}


GPU_Renderer* GPU_GetRenderer(unsigned int index)
{
	if(index >= MAX_ACTIVE_RENDERERS)
		return NULL;
	
	return rendererMap[index];
}

// Get a renderer from the map.
GPU_Renderer* GPU_GetRendererByID(GPU_RendererID id)
{
	GPU_InitRendererRegister();
	
	if(id.index < 0)
		return NULL;
    
    return GPU_GetRenderer(id.index);
}

// Create a new renderer based on a registered id and store it in the map.
GPU_Renderer* GPU_AddRenderer(GPU_RendererID id)
{
	int i;
	for(i = 0; i < MAX_ACTIVE_RENDERERS; i++)
	{
		if(rendererMap[i] == NULL)
		{
			// Create
			GPU_Renderer* renderer = GPU_CreateRenderer(id);
			if(renderer == NULL)
            {
                // TODO: Add more info here
                GPU_LogError("Failed to create new renderer.\n");
                return NULL;
            }
            
			// Add
			rendererMap[i] = renderer;
			renderer->id.index = i;
			// Return
			return renderer;
		}
	}
	
	return NULL;
}

void GPU_FreeRenderer(GPU_Renderer* renderer)
{
	int i;
	for(i = 0; i < MAX_REGISTERED_RENDERERS; i++)
	{
		if(rendererRegister[i].id.id == GPU_RENDERER_UNKNOWN)
			continue;
		
		if(renderer->id.id == rendererRegister[i].id.id)
		{
			rendererRegister[i].freeFn(renderer);
			return;
		}
	}
}

// Remove a renderer from the map and free it.
void GPU_RemoveRenderer(GPU_RendererID id)
{
	int i;
	for(i = 0; i < MAX_ACTIVE_RENDERERS; i++)
	{
		if(rendererMap[i] == NULL)
			continue;
		
		if(i == id.index && id.id == rendererMap[i]->id.id)
		{
			GPU_FreeRenderer(rendererMap[i]);
			rendererMap[i] = NULL;
			return;
		}
	}
}

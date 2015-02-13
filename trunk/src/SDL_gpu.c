#include "SDL_gpu.h"
#include "SDL_gpu_RendererImpl.h"
#include "SDL_platform.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifdef _MSC_VER
	#define __func__ __FUNCTION__
	#pragma warning(push)
	// Visual Studio wants to complain about while(0)
	#pragma warning(disable: 4127)
#endif

#include "stb_image.h"

#ifdef SDL_GPU_USE_SDL2
    #define GET_ALPHA(sdl_color) ((sdl_color).a)
#else
    #define GET_ALPHA(sdl_color) ((sdl_color).unused)
#endif

#define CHECK_RENDERER (current_renderer != NULL)
#define CHECK_CONTEXT (current_renderer->current_context_target != NULL)
#define RETURN_ERROR(code, details) do{ GPU_PushErrorCode(__func__, code, "%s", details); return; } while(0)

int GPU_strcasecmp(const char* s1, const char* s2);

void GPU_InitRendererRegister(void);
GPU_Renderer* GPU_AddRenderer(GPU_RendererID id);
void GPU_RemoveRenderer(GPU_RendererID id);

static GPU_Renderer* current_renderer = NULL;

static GPU_DebugLevelEnum debug_level = GPU_DEBUG_LEVEL_0;

#define GPU_MAX_NUM_ERRORS 20
#define GPU_ERROR_FUNCTION_STRING_MAX 128
#define GPU_ERROR_DETAILS_STRING_MAX 512
static GPU_ErrorObject error_code_stack[GPU_MAX_NUM_ERRORS];
static int num_error_codes = 0;
static int inited_error_code_stack = 0;

/*! A mapping of windowID to a GPU_Target to facilitate GPU_GetWindowTarget(). */
typedef struct GPU_WindowMapping
{
    Uint32 windowID;
    GPU_Target* target;
} GPU_WindowMapping;

#define GPU_INITIAL_WINDOW_MAPPINGS_SIZE 10
static GPU_WindowMapping* window_mappings = NULL;
static int window_mappings_size = 0;
static int num_window_mappings = 0;


SDL_version GPU_GetLinkedVersion(void)
{
    return GPU_GetCompiledVersion();
}

void GPU_SetCurrentRenderer(GPU_RendererID id)
{
	current_renderer = GPU_GetRendererByID(id);
	
	if(current_renderer != NULL)
		current_renderer->impl->SetAsCurrent(current_renderer);
}

void GPU_ResetRendererState(void)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->ResetRendererState(current_renderer);
}

GPU_Renderer* GPU_GetCurrentRenderer(void)
{
	return current_renderer;
}

Uint32 GPU_GetCurrentShaderProgram(void)
{
    if(current_renderer == NULL || current_renderer->current_context_target == NULL)
        return 0;
    
    return current_renderer->current_context_target->context->current_shader_program;
}



void GPU_LogInfo(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	#ifdef __ANDROID__
		__android_log_vprint((GPU_GetDebugLevel() >= GPU_DEBUG_LEVEL_3? ANDROID_LOG_ERROR : ANDROID_LOG_INFO), "APPLICATION", format, args);
	#else
		vfprintf((GPU_GetDebugLevel() >= GPU_DEBUG_LEVEL_3? stderr : stdout), format, args);
	#endif
	va_end(args);
}

void GPU_LogWarning(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	#ifdef __ANDROID__
		__android_log_vprint((GPU_GetDebugLevel() >= GPU_DEBUG_LEVEL_2? ANDROID_LOG_ERROR : ANDROID_LOG_WARN), "APPLICATION", format, args);
	#else
		vfprintf((GPU_GetDebugLevel() >= GPU_DEBUG_LEVEL_2? stderr : stdout), format, args);
	#endif
	va_end(args);
}

void GPU_LogError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	#ifdef __ANDROID__
		__android_log_vprint(ANDROID_LOG_ERROR, "APPLICATION", format, args);
	#else
		vfprintf(stderr, format, args);
	#endif
	va_end(args);
}


static Uint8 init_SDL(void)
{
	if(GPU_GetNumActiveRenderers() == 0)
	{
	    Uint32 subsystems = SDL_WasInit(SDL_INIT_EVERYTHING);
	    if(!subsystems)
        {
            // Nothing has been set up, so init SDL and the video subsystem.
            if(SDL_Init(SDL_INIT_VIDEO) < 0)
            {
                GPU_PushErrorCode("GPU_Init", GPU_ERROR_BACKEND_ERROR, "Failed to initialize SDL video subsystem");
                return 0;
            }
        }
        else if(!(subsystems & SDL_INIT_VIDEO))
        {
            // Something already set up SDL, so just init video.
            if(SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
            {
                GPU_PushErrorCode("GPU_Init", GPU_ERROR_BACKEND_ERROR, "Failed to initialize SDL video subsystem");
                return 0;
            }
        }
	}
	return 1;
}

static Uint32 init_windowID = 0;

void GPU_SetInitWindow(Uint32 windowID)
{
    init_windowID = windowID;
}

Uint32 GPU_GetInitWindow(void)
{
    return init_windowID;
}

static GPU_InitFlagEnum preinit_flags = GPU_DEFAULT_INIT_FLAGS;
static GPU_InitFlagEnum required_features = 0;

void GPU_SetPreInitFlags(GPU_InitFlagEnum GPU_flags)
{
    preinit_flags = GPU_flags;
}

GPU_InitFlagEnum GPU_GetPreInitFlags(void)
{
    return preinit_flags;
}

void GPU_SetRequiredFeatures(GPU_FeatureEnum features)
{
    required_features = features;
}

GPU_FeatureEnum GPU_GetRequiredFeatures(void)
{
    return required_features;
}

static void init_error_stack(void)
{
    if(!inited_error_code_stack)
    {
        int i;
        inited_error_code_stack = 1;
        for(i = 0; i < GPU_MAX_NUM_ERRORS; i++)
        {
            error_code_stack[i].function = (char*)malloc(GPU_ERROR_FUNCTION_STRING_MAX+1);
            error_code_stack[i].details = (char*)malloc(GPU_ERROR_DETAILS_STRING_MAX+1);
        }
    }
}

static void init_window_mappings(void)
{
    if(window_mappings == NULL)
    {
        window_mappings_size = GPU_INITIAL_WINDOW_MAPPINGS_SIZE;
        window_mappings = (GPU_WindowMapping*)malloc(window_mappings_size * sizeof(GPU_WindowMapping));
        num_window_mappings = 0;
    }
}

void GPU_AddWindowMapping(GPU_Target* target)
{
	Uint32 windowID;
	int i;

	if(window_mappings == NULL)
        init_window_mappings();
    
    if(target == NULL || target->context == NULL)
        return;
    
    windowID = target->context->windowID;
    if(windowID == 0)  // Invalid window ID
        return;
    
    // Check for duplicates
    for(i = 0; i < num_window_mappings; i++)
    {
        if(window_mappings[i].windowID == windowID)
        {
            if(window_mappings[i].target != target)
                GPU_PushErrorCode(__func__, GPU_ERROR_DATA_ERROR, "WindowID %u already has a mapping.", windowID);
            return;
        }
        // Don't check the target because it's okay for a single target to be used with multiple windows
    }
    
    // Check if list is big enough to hold another
    if(num_window_mappings >= window_mappings_size)
    {
		GPU_WindowMapping* new_array;
        window_mappings_size *= 2;
        new_array = (GPU_WindowMapping*)malloc(window_mappings_size * sizeof(GPU_WindowMapping));
        memcpy(new_array, window_mappings, num_window_mappings * sizeof(GPU_WindowMapping));
        free(window_mappings);
        window_mappings = new_array;
    }
    
    // Add to end of list
	{
		GPU_WindowMapping m;
		m.windowID = windowID;
		m.target = target;
		window_mappings[num_window_mappings] = m;
	}
    num_window_mappings++;
}

void GPU_RemoveWindowMapping(Uint32 windowID)
{
	int i;

    if(window_mappings == NULL)
        init_window_mappings();
    
    if(windowID == 0)  // Invalid window ID
        return;
    
    // Find the occurrence
    for(i = 0; i < num_window_mappings; i++)
    {
        if(window_mappings[i].windowID == windowID)
        {
			int num_to_move;

            // Unset the target's window
            window_mappings[i].target->context->windowID = 0;
            
            // Move the remaining entries to replace the removed one
            num_window_mappings--;
            num_to_move = num_window_mappings - i;
            if(num_to_move > 0)
                memmove(&window_mappings[i], &window_mappings[i+1], num_to_move * sizeof(GPU_WindowMapping));
            return;
        }
    }
    
}

void GPU_RemoveWindowMappingByTarget(GPU_Target* target)
{
	Uint32 windowID;
	int i;

    if(window_mappings == NULL)
        init_window_mappings();
    
    if(target == NULL || target->context == NULL)
        return;
    
    windowID = target->context->windowID;
    if(windowID == 0)  // Invalid window ID
        return;
    
    // Unset the target's window
    target->context->windowID = 0;
    
    // Find the occurrences
    for(i = 0; i < num_window_mappings; )
    {
        if(window_mappings[i].target == target)
        {
            // Move the remaining entries to replace the removed one
			int num_to_move;
            num_window_mappings--;
            num_to_move = num_window_mappings - i;
            if(num_to_move > 0)
                memmove(&window_mappings[i], &window_mappings[i+1], num_to_move * sizeof(GPU_WindowMapping));
            return;
        }
        else
            i++;
    }
    
}

GPU_Target* GPU_GetWindowTarget(Uint32 windowID)
{
    int i;

    if(window_mappings == NULL)
        init_window_mappings();
    
    if(windowID == 0)  // Invalid window ID
        return NULL;
    
    // Find the occurrence
    for(i = 0; i < num_window_mappings; i++)
    {
        if(window_mappings[i].windowID == windowID)
            return window_mappings[i].target;
    }
    
    return NULL;
}

GPU_Target* GPU_Init(Uint16 w, Uint16 h, GPU_WindowFlagEnum SDL_flags)
{
	int renderer_order_size;
	int i;
    GPU_RendererID renderer_order[GPU_RENDERER_ORDER_MAX];

    init_error_stack();
    
	GPU_InitRendererRegister();
	
	if(!init_SDL())
        return NULL;
        
    renderer_order_size = 0;
    GPU_GetRendererOrder(&renderer_order_size, renderer_order);
	
    // Init the renderers in order
    for(i = 0; i < renderer_order_size; i++)
    {
        GPU_Target* screen = GPU_InitRendererByID(renderer_order[i], w, h, SDL_flags);
        if(screen != NULL)
            return screen;
    }
    
    return NULL;
}

GPU_Target* GPU_InitRenderer(GPU_RendererEnum renderer_enum, Uint16 w, Uint16 h, GPU_WindowFlagEnum SDL_flags)
{
    // Search registry for this renderer and use that id
    return GPU_InitRendererByID(GPU_GetRendererID(renderer_enum), w, h, SDL_flags);
}

GPU_Target* GPU_InitRendererByID(GPU_RendererID renderer_request, Uint16 w, Uint16 h, GPU_WindowFlagEnum SDL_flags)
{
	GPU_Renderer* renderer;
	GPU_Target* screen;

    init_error_stack();
	GPU_InitRendererRegister();
	
	if(!init_SDL())
        return NULL;
	
	renderer = GPU_AddRenderer(renderer_request);
	if(renderer == NULL)
		return NULL;
    
	GPU_SetCurrentRenderer(renderer->id);
	
	screen = renderer->impl->Init(renderer, renderer_request, w, h, SDL_flags);
	if(screen == NULL)
    {
        // Init failed, destroy the renderer...
        // Erase the window mappings
        num_window_mappings = 0;
        GPU_CloseCurrentRenderer();
    }
    else
        GPU_SetInitWindow(0);
    return screen;
}

Uint8 GPU_IsFeatureEnabled(GPU_FeatureEnum feature)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
    
	return ((current_renderer->enabled_features & feature) == feature);
}

GPU_Target* GPU_CreateTargetFromWindow(Uint32 windowID)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CreateTargetFromWindow(current_renderer, windowID, NULL);
}

GPU_Target* GPU_CreateAliasTarget(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CreateAliasTarget(current_renderer, target);
}

void GPU_MakeCurrent(GPU_Target* target, Uint32 windowID)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->MakeCurrent(current_renderer, target, windowID);
}

Uint8 GPU_SetFullscreen(Uint8 enable_fullscreen, Uint8 use_desktop_resolution)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->SetFullscreen(current_renderer, enable_fullscreen, use_desktop_resolution);
}

Uint8 GPU_GetFullscreen(void)
{
#ifdef SDL_GPU_USE_SDL2
    GPU_Target* target = GPU_GetContextTarget();
    if(target == NULL)
        return 0;
    return (Uint8)(SDL_GetWindowFlags(SDL_GetWindowFromID(target->context->windowID))
             & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP));
#else
    SDL_Surface* surf = SDL_GetVideoSurface();
    if(surf == NULL)
        return 0;
    return (surf->flags & SDL_FULLSCREEN);
#endif
}

Uint8 GPU_SetWindowResolution(Uint16 w, Uint16 h)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL || w == 0 || h == 0)
		return 0;
	
	return current_renderer->impl->SetWindowResolution(current_renderer, w, h);
}


void GPU_SetVirtualResolution(GPU_Target* target, Uint16 w, Uint16 h)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL || w == 0 || h == 0)
		return;
	
	current_renderer->impl->SetVirtualResolution(current_renderer, target, w, h);
}

void GPU_UnsetVirtualResolution(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->UnsetVirtualResolution(current_renderer, target);
}

void GPU_CloseCurrentRenderer(void)
{
	if(current_renderer == NULL)
		return;
	
	current_renderer->impl->Quit(current_renderer);
	GPU_RemoveRenderer(current_renderer->id);
	current_renderer = NULL;
}

void GPU_Quit(void)
{
    int i;
    if(num_error_codes > 0 && GPU_GetDebugLevel() >= GPU_DEBUG_LEVEL_1)
        GPU_LogError("GPU_Quit: %d uncleared errors.\n", num_error_codes);
        
    // Free the error stack
    for(i = 0; i < GPU_MAX_NUM_ERRORS; i++)
    {
        free(error_code_stack[i].function);
        error_code_stack[i].function = NULL;
        free(error_code_stack[i].details);
        error_code_stack[i].details = NULL;
    }
    inited_error_code_stack = 0;
    
	// FIXME: Remove all renderers
	if(current_renderer == NULL)
		return;
	
	current_renderer->impl->Quit(current_renderer);
	GPU_RemoveRenderer(current_renderer->id);
	current_renderer = NULL;
	
	if(GPU_GetNumActiveRenderers() == 0)
		SDL_Quit();
}

void GPU_SetDebugLevel(GPU_DebugLevelEnum level)
{
    if(level > GPU_DEBUG_LEVEL_MAX)
        level = GPU_DEBUG_LEVEL_MAX;
    debug_level = level;
}

GPU_DebugLevelEnum GPU_GetDebugLevel(void)
{
    return debug_level;
}

void GPU_PushErrorCode(const char* function, GPU_ErrorEnum error, const char* details, ...)
{
    if(GPU_GetDebugLevel() >= GPU_DEBUG_LEVEL_1)
    {
        // Print the message
        if(details != NULL)
        {
            char buf[GPU_ERROR_DETAILS_STRING_MAX];
            va_list lst;
            va_start(lst, details);
            vsnprintf(buf, GPU_ERROR_DETAILS_STRING_MAX, details, lst);
            va_end(lst);
            
            GPU_LogError("%s: %s - %s\n", (function == NULL? "NULL" : function), GPU_GetErrorString(error), buf);
        }
        else
            GPU_LogError("%s: %s\n", (function == NULL? "NULL" : function), GPU_GetErrorString(error));
    }
    
    if(num_error_codes < GPU_MAX_NUM_ERRORS)
    {
        if(function == NULL)
            error_code_stack[num_error_codes].function[0] = '\0';
        else
        {
            strncpy(error_code_stack[num_error_codes].function, function, GPU_ERROR_FUNCTION_STRING_MAX);
            error_code_stack[num_error_codes].function[GPU_ERROR_FUNCTION_STRING_MAX] = '\0';
        }
        error_code_stack[num_error_codes].error = error;
        if(details == NULL)
            error_code_stack[num_error_codes].details[0] = '\0';
        else
        {
            va_list lst;
            va_start(lst, details);
            vsnprintf(error_code_stack[num_error_codes].details, GPU_ERROR_DETAILS_STRING_MAX, details, lst);
            va_end(lst);
        }
        num_error_codes++;
    }
}

GPU_ErrorObject GPU_PopErrorCode(void)
{
    if(num_error_codes <= 0)
    {
        GPU_ErrorObject e = {NULL, GPU_ERROR_NONE, NULL};
        return e;
    }
    
    return error_code_stack[--num_error_codes];
}

const char* GPU_GetErrorString(GPU_ErrorEnum error)
{
    switch(error)
    {
        case GPU_ERROR_NONE:
            return "NO ERROR";
        case GPU_ERROR_BACKEND_ERROR:
            return "BACKEND ERROR";
        case GPU_ERROR_DATA_ERROR:
            return "DATA ERROR";
        case GPU_ERROR_USER_ERROR:
            return "USER ERROR";
        case GPU_ERROR_UNSUPPORTED_FUNCTION:
            return "UNSUPPORTED FUNCTION";
        case GPU_ERROR_NULL_ARGUMENT:
            return "NULL ARGUMENT";
        case GPU_ERROR_FILE_NOT_FOUND:
            return "FILE NOT FOUND";
    }
    return "UNKNOWN ERROR";
}


void GPU_GetVirtualCoords(GPU_Target* target, float* x, float* y, float displayX, float displayY)
{
	if(target == NULL)
		return;
	
	if(target->context != NULL)
    {
        if(x != NULL)
            *x = (displayX*target->w)/target->context->window_w;
        if(y != NULL)
            *y = (displayY*target->h)/target->context->window_h;
    }
	else if(target->image != NULL)
    {
        if(x != NULL)
            *x = (displayX*target->w)/target->image->w;
        if(y != NULL)
            *y = (displayY*target->h)/target->image->h;
    }
    else
    {
        if(x != NULL)
            *x = displayX;
        if(y != NULL)
            *y = displayY;
    }
}

GPU_Rect GPU_MakeRect(float x, float y, float w, float h)
{
	GPU_Rect r;
	r.x = x;
	r.y = y;
	r.w = w;
	r.h = h;

    return r;
}

SDL_Color GPU_MakeColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	SDL_Color c;
	c.r = r;
	c.g = g;
	c.b = b;
	c.a = a;

    return c;
}

GPU_RendererID GPU_MakeRendererID(const char* name, GPU_RendererEnum renderer, int major_version, int minor_version)
{
	GPU_RendererID r;
	r.name = name;
	r.renderer = renderer;
	r.major_version = major_version;
	r.minor_version = minor_version;
	r.index = -1;
	
    return r;
}

void GPU_SetViewport(GPU_Target* target, GPU_Rect viewport)
{
    if(target != NULL)
        target->viewport = viewport;
}

GPU_Camera GPU_GetDefaultCamera(void)
{
	GPU_Camera cam = {0.0f, 0.0f, -10.0f, 0.0f, 1.0f};
	return cam;
}

GPU_Camera GPU_GetCamera(GPU_Target* target)
{
	if(target == NULL)
		return GPU_GetDefaultCamera();
	return target->camera;
}

GPU_Camera GPU_SetCamera(GPU_Target* target, GPU_Camera* cam)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return GPU_GetDefaultCamera();
	
	return current_renderer->impl->SetCamera(current_renderer, target, cam);
}

GPU_Image* GPU_CreateImage(Uint16 w, Uint16 h, GPU_FormatEnum format)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CreateImage(current_renderer, w, h, format);
}

GPU_Image* GPU_CreateImageUsingTexture(Uint32 handle, Uint8 take_ownership)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CreateImageUsingTexture(current_renderer, handle, take_ownership);
}

GPU_Image* GPU_LoadImage(const char* filename)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->LoadImage(current_renderer, filename);
}

GPU_Image* GPU_CreateAliasImage(GPU_Image* image)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CreateAliasImage(current_renderer, image);
}

Uint8 GPU_SaveImage(GPU_Image* image, const char* filename, GPU_FileFormatEnum format)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->SaveImage(current_renderer, image, filename, format);
}

GPU_Image* GPU_CopyImage(GPU_Image* image)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CopyImage(current_renderer, image);
}

void GPU_UpdateImage(GPU_Image* image, SDL_Surface* surface, const GPU_Rect* surface_rect)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->UpdateImage(current_renderer, image, surface, surface_rect);
}

void GPU_UpdateSubImage(GPU_Image* image, const GPU_Rect* image_rect, SDL_Surface* surface, const GPU_Rect* surface_rect)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->UpdateSubImage(current_renderer, image, image_rect, surface, surface_rect);
}

void GPU_UpdateImageBytes(GPU_Image* image, const GPU_Rect* image_rect, const unsigned char* bytes, int bytes_per_row)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->UpdateImageBytes(current_renderer, image, image_rect, bytes, bytes_per_row);
}

SDL_Surface* GPU_LoadSurface(const char* filename)
{
	int width, height, channels;
	Uint32 Rmask, Gmask, Bmask, Amask = 0;
	unsigned char* data;
	SDL_Surface* result;
	
	if(filename == NULL)
    {
        GPU_PushErrorCode("GPU_LoadSurface", GPU_ERROR_NULL_ARGUMENT, "filename");
        return NULL;
    }
	
	#ifdef __ANDROID__
	if(strlen(filename) > 0 && filename[0] != '/')
	{
        // Must use SDL_RWops to access the assets directory automatically
        SDL_RWops* rwops = SDL_RWFromFile(filename, "r");
        if(rwops == NULL)
            return NULL;
        int data_bytes = SDL_RWseek(rwops, 0, SEEK_END);
        SDL_RWseek(rwops, 0, SEEK_SET);
        unsigned char* c_data = (unsigned char*)malloc(data_bytes);
        SDL_RWread(rwops, c_data, 1, data_bytes);
        data = stbi_load_from_memory(c_data, data_bytes, &width, &height, &channels, 0);
        free(c_data);
        SDL_FreeRW(rwops);
	}
	else
    {
        // Absolute filename
        data = stbi_load(filename, &width, &height, &channels, 0);
    }
	#else
	data = stbi_load(filename, &width, &height, &channels, 0);
	#endif
	
	if(data == NULL)
	{
		GPU_PushErrorCode(__func__, GPU_ERROR_DATA_ERROR, "Failed to load \"%s\": %s", filename, stbi_failure_reason());
		return NULL;
	}
	if(channels < 1 || channels > 4)
	{
		GPU_PushErrorCode(__func__, GPU_ERROR_DATA_ERROR, "Failed to load \"%s\": Unsupported pixel format", filename);
		stbi_image_free(data);
		return NULL;
	}
	
	switch(channels)
	{
        case 1:
            Rmask = Gmask = Bmask = 0;  // Use default RGB masks for 8-bit
            break;
        case 2:
            Rmask = Gmask = Bmask = 0;  // Use default RGB masks for 16-bit
            break;
        case 3:
            // These are reversed from what SDL_image uses...  That is bad. :(  Needs testing.
            #if SDL_BYTEORDER == SDL_BIG_ENDIAN
            Rmask = 0xff0000;
            Gmask = 0x00ff00;
            Bmask = 0x0000ff;
            #else
            Rmask = 0x0000ff;
            Gmask = 0x00ff00;
            Bmask = 0xff0000;
            #endif
            break;
        case 4:
            Rmask = 0x000000ff;
            Gmask = 0x0000ff00;
            Bmask = 0x00ff0000;
            Amask = 0xff000000;
            break;
	}
	
	result = SDL_CreateRGBSurfaceFrom(data, width, height, channels*8, width*channels, Rmask, Gmask, Bmask, Amask);
	if(result != NULL)
        result->flags &= ~SDL_PREALLOC;  // Make SDL take ownership of the data memory
    
	if(result != NULL && result->format->palette != NULL)
    {
        // SDL_CreateRGBSurface has no idea what palette to use, so it uses a blank one.
        // We'll at least create a grayscale one, but it's not ideal...
        // Better would be to get the palette from stbi, but stbi doesn't do that!
        SDL_Color colors[256];
        int i;
        
        for(i = 0; i < 256; i++)
        {
            colors[i].r = colors[i].g = colors[i].b = (Uint8)i;
        }

        /* Set palette */
        #ifdef SDL_GPU_USE_SDL2
        SDL_SetPaletteColors(result->format->palette, colors, 0, 256);
        #else
        SDL_SetPalette(result, SDL_LOGPAL, colors, 0, 256);
        #endif
    }
	
	return result;
}

#include "stb_image.h"
#include "stb_image_write.h"

// From http://stackoverflow.com/questions/5309471/getting-file-extension-in-c
static const char *get_filename_ext(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename)
        return "";
    return dot + 1;
}

Uint8 GPU_SaveSurface(SDL_Surface* surface, const char* filename, GPU_FileFormatEnum format)
{
    Uint8 result;
    unsigned char* data;

    if(surface == NULL || filename == NULL ||
            surface->w < 1 || surface->h < 1)
    {
        return 0;
    }


    data = surface->pixels;
    
    if(format == GPU_FILE_AUTO)
    {
        const char* extension = get_filename_ext(filename);
        if(GPU_strcasecmp(extension, "png") == 0)
            format = GPU_FILE_PNG;
        else if(GPU_strcasecmp(extension, "bmp") == 0)
            format = GPU_FILE_BMP;
        else if(GPU_strcasecmp(extension, "tga") == 0)
            format = GPU_FILE_TGA;
        else
        {
            GPU_PushErrorCode(__func__, GPU_ERROR_DATA_ERROR, "Could not detect output file format from file name");
            return 0;
        }
    }
    
    switch(format)
    {
        case GPU_FILE_PNG:
            result = (stbi_write_png(filename, surface->w, surface->h, surface->format->BytesPerPixel, (const unsigned char *const)data, 0) > 0);
            break;
        case GPU_FILE_BMP:
            result = (stbi_write_bmp(filename, surface->w, surface->h, surface->format->BytesPerPixel, (void*)data) > 0);
            break;
        case GPU_FILE_TGA:
            result = (stbi_write_tga(filename, surface->w, surface->h, surface->format->BytesPerPixel, (void*)data) > 0);
            break;
        default:
            GPU_PushErrorCode(__func__, GPU_ERROR_DATA_ERROR, "Unsupported output file format");
            result = 0;
            break;
    }

    return result;
}

GPU_Image* GPU_CopyImageFromSurface(SDL_Surface* surface)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CopyImageFromSurface(current_renderer, surface);
}

GPU_Image* GPU_CopyImageFromTarget(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CopyImageFromTarget(current_renderer, target);
}

SDL_Surface* GPU_CopySurfaceFromTarget(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CopySurfaceFromTarget(current_renderer, target);
}

SDL_Surface* GPU_CopySurfaceFromImage(GPU_Image* image)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->CopySurfaceFromImage(current_renderer, image);
}

void GPU_FreeImage(GPU_Image* image)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->FreeImage(current_renderer, image);
}


GPU_Target* GPU_GetContextTarget(void)
{
	if(current_renderer == NULL)
		return NULL;
	
	return current_renderer->current_context_target;
}


GPU_Target* GPU_LoadTarget(GPU_Image* image)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->LoadTarget(current_renderer, image);
}



void GPU_FreeTarget(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->FreeTarget(current_renderer, target);
}



void GPU_Blit(GPU_Image* image, GPU_Rect* src_rect, GPU_Target* target, float x, float y)
{
    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
	
	current_renderer->impl->Blit(current_renderer, image, src_rect, target, x, y);
}


void GPU_BlitRotate(GPU_Image* image, GPU_Rect* src_rect, GPU_Target* target, float x, float y, float angle)
{
    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
	
	current_renderer->impl->BlitRotate(current_renderer, image, src_rect, target, x, y, angle);
}

void GPU_BlitScale(GPU_Image* image, GPU_Rect* src_rect, GPU_Target* target, float x, float y, float scaleX, float scaleY)
{
    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
	
	current_renderer->impl->BlitScale(current_renderer, image, src_rect, target, x, y, scaleX, scaleY);
}

void GPU_BlitTransform(GPU_Image* image, GPU_Rect* src_rect, GPU_Target* target, float x, float y, float angle, float scaleX, float scaleY)
{
    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
	
	current_renderer->impl->BlitTransform(current_renderer, image, src_rect, target, x, y, angle, scaleX, scaleY);
}

void GPU_BlitTransformX(GPU_Image* image, GPU_Rect* src_rect, GPU_Target* target, float x, float y, float pivot_x, float pivot_y, float angle, float scaleX, float scaleY)
{
    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
	
	current_renderer->impl->BlitTransformX(current_renderer, image, src_rect, target, x, y, pivot_x, pivot_y, angle, scaleX, scaleY);
}

void GPU_BlitTransformMatrix(GPU_Image* image, GPU_Rect* src_rect, GPU_Target* target, float x, float y, float* matrix3x3)
{
    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
    
    if(matrix3x3 == NULL)
		return;
	
	current_renderer->impl->BlitTransformMatrix(current_renderer, image, src_rect, target, x, y, matrix3x3);
}

void GPU_BlitBatch(GPU_Image* image, GPU_Target* target, unsigned int num_sprites, float* values, GPU_BlitFlagEnum flags)
{
	int src_position_floats_per_sprite;
	int src_rect_floats_per_sprite;
	int src_color_floats_per_sprite;

	Uint8 no_positions;
	Uint8 no_rects;
	Uint8 no_colors;
	Uint8 pass_vertices;
	Uint8 pass_texcoords;
	Uint8 pass_colors;

	int src_floats_per_sprite;

	int size;
	float* new_values;

	unsigned int n;  // The sprite number iteration variable.
	// Source indices (per sprite)
	int pos_n;
	int rect_n;
	int color_n;
	// Dest indices
	int vert_i;
	int texcoord_i;
	int color_i;
	// Dest float stride
	int floats_per_vertex;

	float w2;  // texcoord helpers for position expansion
	float h2;

	Uint32 tex_w;
	Uint32 tex_h;

    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
    
    if(num_sprites == 0)
        return;
    
    // Is it already in the right format?
    if((flags & GPU_PASSTHROUGH_ALL) == GPU_PASSTHROUGH_ALL || values == NULL)
    {
        current_renderer->impl->BlitBatch(current_renderer, image, target, num_sprites, values, flags);
        return;
    }
	
	// Conversion time...
	
	// Convert condensed interleaved format into full interleaved format for the renderer to use.
	// Condensed: Each vertex has 2 pos, 4 rect, 4 color
	
	// Default values: Each sprite is defined by a position, a rect, and a color.
	src_position_floats_per_sprite = 2;
	src_rect_floats_per_sprite = 4;
	src_color_floats_per_sprite = 4;
	
	no_positions = (Uint8)(flags & GPU_USE_DEFAULT_POSITIONS);
	no_rects = (Uint8)(flags & GPU_USE_DEFAULT_SRC_RECTS);
	no_colors = (Uint8)(flags & GPU_USE_DEFAULT_COLORS);
	pass_vertices = (Uint8)(flags & GPU_PASSTHROUGH_VERTICES);
	pass_texcoords = (Uint8)(flags & GPU_PASSTHROUGH_TEXCOORDS);
	pass_colors = (Uint8)(flags & GPU_PASSTHROUGH_COLORS);
	
	// Passthrough data is per-vertex.  Non-passthrough is per-sprite.  They can't interleave cleanly.
	if(flags & GPU_PASSTHROUGH_ALL && (flags & GPU_PASSTHROUGH_ALL) != GPU_PASSTHROUGH_ALL)
    {
        GPU_PushErrorCode(__func__, GPU_ERROR_USER_ERROR, "Cannot interpret interleaved data using partial passthrough");
        return;
    }
	
	if(pass_vertices)
        src_position_floats_per_sprite = 8; // 4 vertices of x, y
	if(pass_texcoords)
        src_rect_floats_per_sprite = 8; // 4 vertices of s, t
	if(pass_colors)
        src_color_floats_per_sprite = 16; // 4 vertices of r, g, b, a
	if(no_positions)
        src_position_floats_per_sprite = 0;
	if(no_rects)
        src_rect_floats_per_sprite = 0;
	if(no_colors)
        src_color_floats_per_sprite = 0;
    
	src_floats_per_sprite = src_position_floats_per_sprite + src_rect_floats_per_sprite + src_color_floats_per_sprite;
	
	size = num_sprites*(8 + 8 + 16);
	new_values = (float*)malloc(sizeof(float)*size);
    
	// Source indices (per sprite)
	pos_n = 0;
	rect_n = src_position_floats_per_sprite;
	color_n = src_position_floats_per_sprite + src_rect_floats_per_sprite;
	// Dest indices
	vert_i = 0;
	texcoord_i = 2;
	color_i = 4;
	// Dest float stride
	floats_per_vertex = 8;
	
	w2 = 0.5f*image->w;  // texcoord helpers for position expansion
	h2 = 0.5f*image->h;
	
	tex_w = image->base_w;
	tex_h = image->base_h;
	
    for(n = 0; n < num_sprites; n++)
    {
        if(no_rects)
        {
            new_values[texcoord_i] = 0.0f;
            new_values[texcoord_i+1] = 0.0f;
            texcoord_i += floats_per_vertex;
            new_values[texcoord_i] = 1.0f;
            new_values[texcoord_i+1] = 0.0f;
            texcoord_i += floats_per_vertex;
            new_values[texcoord_i] = 1.0f;
            new_values[texcoord_i+1] = 1.0f;
            texcoord_i += floats_per_vertex;
            new_values[texcoord_i] = 0.0f;
            new_values[texcoord_i+1] = 1.0f;
            texcoord_i += floats_per_vertex;
        }
        else
        {
            if(!pass_texcoords)
            {
                float s1 = values[rect_n]/tex_w;
                float t1 = values[rect_n+1]/tex_h;
                float s3 = s1 + values[rect_n+2]/tex_w;
                float t3 = t1 + values[rect_n+3]/tex_h;
                rect_n += src_floats_per_sprite;
                
                new_values[texcoord_i] = s1;
                new_values[texcoord_i+1] = t1;
                texcoord_i += floats_per_vertex;
                new_values[texcoord_i] = s3;
                new_values[texcoord_i+1] = t1;
                texcoord_i += floats_per_vertex;
                new_values[texcoord_i] = s3;
                new_values[texcoord_i+1] = t3;
                texcoord_i += floats_per_vertex;
                new_values[texcoord_i] = s1;
                new_values[texcoord_i+1] = t3;
                texcoord_i += floats_per_vertex;
            
                if(!pass_vertices)
                {
                    w2 = 0.5f*(s3-s1)*image->w;
                    h2 = 0.5f*(t3-t1)*image->h;
                }
            }
            else
            {
                // 4 vertices all in a row
				float s1, t1, s3, t3;
                s1 = new_values[texcoord_i] = values[rect_n];
                t1 = new_values[texcoord_i+1] = values[rect_n+1];
                texcoord_i += floats_per_vertex;
                new_values[texcoord_i] = values[rect_n+2];
                new_values[texcoord_i+1] = values[rect_n+3];
                texcoord_i += floats_per_vertex;
                s3 = new_values[texcoord_i] = values[rect_n+4];
                t3 = new_values[texcoord_i+1] = values[rect_n+5];
                texcoord_i += floats_per_vertex;
                new_values[texcoord_i] = values[rect_n+6];
                new_values[texcoord_i+1] = values[rect_n+7];
                texcoord_i += floats_per_vertex;
                rect_n += src_floats_per_sprite;
            
                if(!pass_vertices)
                {
                    w2 = 0.5f*(s3-s1)*image->w;
                    h2 = 0.5f*(t3-t1)*image->h;
                }
            }
        }
        
        if(no_positions)
        {
            new_values[vert_i] = 0.0f;
            new_values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
            new_values[vert_i] = 0.0f;
            new_values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
            new_values[vert_i] = 0.0f;
            new_values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
            new_values[vert_i] = 0.0f;
            new_values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
        }
        else
        {
            if(!pass_vertices)
            {
                // Expand vertices from the position and dimensions
                float x = values[pos_n];
                float y = values[pos_n+1];
                pos_n += src_floats_per_sprite;
                
                new_values[vert_i] = x - w2;
                new_values[vert_i+1] = y - h2;
                vert_i += floats_per_vertex;
                new_values[vert_i] = x + w2;
                new_values[vert_i+1] = y - h2;
                vert_i += floats_per_vertex;
                new_values[vert_i] = x + w2;
                new_values[vert_i+1] = y + h2;
                vert_i += floats_per_vertex;
                new_values[vert_i] = x - w2;
                new_values[vert_i+1] = y + h2;
                vert_i += floats_per_vertex;
            }
            else
            {
                // 4 vertices all in a row
                new_values[vert_i] = values[pos_n];
                new_values[vert_i+1] = values[pos_n+1];
                vert_i += floats_per_vertex;
                new_values[vert_i] = values[pos_n+2];
                new_values[vert_i+1] = values[pos_n+3];
                vert_i += floats_per_vertex;
                new_values[vert_i] = values[pos_n+4];
                new_values[vert_i+1] = values[pos_n+5];
                vert_i += floats_per_vertex;
                new_values[vert_i] = values[pos_n+6];
                new_values[vert_i+1] = values[pos_n+7];
                vert_i += floats_per_vertex;
                pos_n += src_floats_per_sprite;
            }
        }
        
        if(no_colors)
        {
                new_values[color_i] = 1.0f;
                new_values[color_i+1] = 1.0f;
                new_values[color_i+2] = 1.0f;
                new_values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
                new_values[color_i] = 1.0f;
                new_values[color_i+1] = 1.0f;
                new_values[color_i+2] = 1.0f;
                new_values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
                new_values[color_i] = 1.0f;
                new_values[color_i+1] = 1.0f;
                new_values[color_i+2] = 1.0f;
                new_values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
                new_values[color_i] = 1.0f;
                new_values[color_i+1] = 1.0f;
                new_values[color_i+2] = 1.0f;
                new_values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
        }
        else
        {
            if(!pass_colors)
            {
                float r = values[color_n]/255.0f;
                float g = values[color_n+1]/255.0f;
                float b = values[color_n+2]/255.0f;
                float a = values[color_n+3]/255.0f;
                color_n += src_floats_per_sprite;
                
                new_values[color_i] = r;
                new_values[color_i+1] = g;
                new_values[color_i+2] = b;
                new_values[color_i+3] = a;
                color_i += floats_per_vertex;
                new_values[color_i] = r;
                new_values[color_i+1] = g;
                new_values[color_i+2] = b;
                new_values[color_i+3] = a;
                color_i += floats_per_vertex;
                new_values[color_i] = r;
                new_values[color_i+1] = g;
                new_values[color_i+2] = b;
                new_values[color_i+3] = a;
                color_i += floats_per_vertex;
                new_values[color_i] = r;
                new_values[color_i+1] = g;
                new_values[color_i+2] = b;
                new_values[color_i+3] = a;
                color_i += floats_per_vertex;
            }
            else
            {
                // 4 vertices all in a row
                new_values[color_i] = values[color_n];
                new_values[color_i+1] = values[color_n+1];
                new_values[color_i+2] = values[color_n+2];
                new_values[color_i+3] = values[color_n+3];
                color_i += floats_per_vertex;
                new_values[color_i] = values[color_n+4];
                new_values[color_i+1] = values[color_n+5];
                new_values[color_i+2] = values[color_n+6];
                new_values[color_i+3] = values[color_n+7];
                color_i += floats_per_vertex;
                new_values[color_i] = values[color_n+8];
                new_values[color_i+1] = values[color_n+9];
                new_values[color_i+2] = values[color_n+10];
                new_values[color_i+3] = values[color_n+11];
                color_i += floats_per_vertex;
                new_values[color_i] = values[color_n+12];
                new_values[color_i+1] = values[color_n+13];
                new_values[color_i+2] = values[color_n+14];
                new_values[color_i+3] = values[color_n+15];
                color_i += floats_per_vertex;
                color_n += src_floats_per_sprite;
            }
        }
    }
    
	current_renderer->impl->BlitBatch(current_renderer, image, target, num_sprites, new_values, flags | GPU_PASSTHROUGH_ALL);
	
	free(new_values);
}

void GPU_BlitBatchSeparate(GPU_Image* image, GPU_Target* target, unsigned int num_sprites, float* positions, float* src_rects, float* colors, GPU_BlitFlagEnum flags)
{
	Uint8 pass_vertices;
	Uint8 pass_texcoords;
	Uint8 pass_colors;

	int size;  // 4 vertices of x, y...  s, t...  r, g, b, a
	float* values;

	unsigned int n;  // The sprite number iteration variable.
	// Source indices
	int pos_n;
	int rect_n;
	int color_n;
	// Dest indices
	int vert_i;
	int texcoord_i;
	int color_i;
	// Dest float stride
	int floats_per_vertex;

	float w2;  // texcoord helpers for position expansion
	float h2;

	Uint32 tex_w;
	Uint32 tex_h;

    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(image == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "image");
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
    
    if(num_sprites == 0)
        return;
    
    // No data to repack?  Skip it.
    if(positions == NULL && src_rects == NULL && colors == NULL)
    {
        current_renderer->impl->BlitBatch(current_renderer, image, target, num_sprites, NULL, flags);
        return;
    }
	
	// Repack the given arrays into an interleaved array for more efficient access
	// Default values: Each sprite is defined by a position, a rect, and a color.
	
	pass_vertices = (Uint8)(flags & GPU_PASSTHROUGH_VERTICES);
	pass_texcoords = (Uint8)(flags & GPU_PASSTHROUGH_TEXCOORDS);
	pass_colors = (Uint8)(flags & GPU_PASSTHROUGH_COLORS);
	
	size = num_sprites*(8 + 8 + 16);  // 4 vertices of x, y...  s, t...  r, g, b, a
	values = (float*)malloc(sizeof(float)*size);
	
	// Source indices
	pos_n = 0;
	rect_n = 0;
	color_n = 0;
	// Dest indices
	vert_i = 0;
	texcoord_i = 2;
	color_i = 4;
	// Dest float stride
	floats_per_vertex = 8;
	
	w2 = 0.5f*image->w;  // texcoord helpers for position expansion
	h2 = 0.5f*image->h;
	
	tex_w = image->base_w;
	tex_h = image->base_h;
    
	for(n = 0; n < num_sprites; n++)
    {
        // Unpack the arrays
        
        if(src_rects == NULL)
        {
            values[texcoord_i] = 0.0f;
            values[texcoord_i+1] = 0.0f;
            texcoord_i += floats_per_vertex;
            values[texcoord_i] = 1.0f;
            values[texcoord_i+1] = 0.0f;
            texcoord_i += floats_per_vertex;
            values[texcoord_i] = 1.0f;
            values[texcoord_i+1] = 1.0f;
            texcoord_i += floats_per_vertex;
            values[texcoord_i] = 0.0f;
            values[texcoord_i+1] = 1.0f;
            texcoord_i += floats_per_vertex;
        }
        else
        {
            if(!pass_texcoords)
            {
                float s1 = src_rects[rect_n++]/tex_w;
                float t1 = src_rects[rect_n++]/tex_h;
                float s3 = s1 + src_rects[rect_n++]/tex_w;
                float t3 = t1 + src_rects[rect_n++]/tex_h;
                
                values[texcoord_i] = s1;
                values[texcoord_i+1] = t1;
                texcoord_i += floats_per_vertex;
                values[texcoord_i] = s3;
                values[texcoord_i+1] = t1;
                texcoord_i += floats_per_vertex;
                values[texcoord_i] = s3;
                values[texcoord_i+1] = t3;
                texcoord_i += floats_per_vertex;
                values[texcoord_i] = s1;
                values[texcoord_i+1] = t3;
                texcoord_i += floats_per_vertex;
            
                if(!pass_vertices)
                {
                    w2 = 0.5f*(s3-s1)*image->w;
                    h2 = 0.5f*(t3-t1)*image->h;
                }
            }
            else
            {
                // 4 vertices all in a row
				float s1, t1, s3, t3;
                s1 = values[texcoord_i] = src_rects[rect_n++];
                t1 = values[texcoord_i+1] = src_rects[rect_n++];
                texcoord_i += floats_per_vertex;
                values[texcoord_i] = src_rects[rect_n++];
                values[texcoord_i+1] = src_rects[rect_n++];
                texcoord_i += floats_per_vertex;
                s3 = values[texcoord_i] = src_rects[rect_n++];
                t3 = values[texcoord_i+1] = src_rects[rect_n++];
                texcoord_i += floats_per_vertex;
                values[texcoord_i] = src_rects[rect_n++];
                values[texcoord_i+1] = src_rects[rect_n++];
                texcoord_i += floats_per_vertex;
            
                if(!pass_vertices)
                {
                    w2 = 0.5f*(s3-s1)*image->w;
                    h2 = 0.5f*(t3-t1)*image->h;
                }
            }
        }
        
        if(positions == NULL)
        {
            values[vert_i] = 0.0f;
            values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
            values[vert_i] = 0.0f;
            values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
            values[vert_i] = 0.0f;
            values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
            values[vert_i] = 0.0f;
            values[vert_i+1] = 0.0f;
            vert_i += floats_per_vertex;
        }
        else
        {
            if(!pass_vertices)
            {
                // Expand vertices from the position and dimensions
                float x = positions[pos_n++];
                float y = positions[pos_n++];
                values[vert_i] = x - w2;
                values[vert_i+1] = y - h2;
                vert_i += floats_per_vertex;
                values[vert_i] = x + w2;
                values[vert_i+1] = y - h2;
                vert_i += floats_per_vertex;
                values[vert_i] = x + w2;
                values[vert_i+1] = y + h2;
                vert_i += floats_per_vertex;
                values[vert_i] = x - w2;
                values[vert_i+1] = y + h2;
                vert_i += floats_per_vertex;
            }
            else
            {
                // 4 vertices all in a row
                values[vert_i] = positions[pos_n++];
                values[vert_i+1] = positions[pos_n++];
                vert_i += floats_per_vertex;
                values[vert_i] = positions[pos_n++];
                values[vert_i+1] = positions[pos_n++];
                vert_i += floats_per_vertex;
                values[vert_i] = positions[pos_n++];
                values[vert_i+1] = positions[pos_n++];
                vert_i += floats_per_vertex;
                values[vert_i] = positions[pos_n++];
                values[vert_i+1] = positions[pos_n++];
                vert_i += floats_per_vertex;
            }
        }
        
        if(colors == NULL)
        {
                values[color_i] = 1.0f;
                values[color_i+1] = 1.0f;
                values[color_i+2] = 1.0f;
                values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
                values[color_i] = 1.0f;
                values[color_i+1] = 1.0f;
                values[color_i+2] = 1.0f;
                values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
                values[color_i] = 1.0f;
                values[color_i+1] = 1.0f;
                values[color_i+2] = 1.0f;
                values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
                values[color_i] = 1.0f;
                values[color_i+1] = 1.0f;
                values[color_i+2] = 1.0f;
                values[color_i+3] = 1.0f;
                color_i += floats_per_vertex;
        }
        else
        {
            if(!pass_colors)
            {
                float r = colors[color_n++]/255.0f;
                float g = colors[color_n++]/255.0f;
                float b = colors[color_n++]/255.0f;
                float a = colors[color_n++]/255.0f;
                
                values[color_i] = r;
                values[color_i+1] = g;
                values[color_i+2] = b;
                values[color_i+3] = a;
                color_i += floats_per_vertex;
                values[color_i] = r;
                values[color_i+1] = g;
                values[color_i+2] = b;
                values[color_i+3] = a;
                color_i += floats_per_vertex;
                values[color_i] = r;
                values[color_i+1] = g;
                values[color_i+2] = b;
                values[color_i+3] = a;
                color_i += floats_per_vertex;
                values[color_i] = r;
                values[color_i+1] = g;
                values[color_i+2] = b;
                values[color_i+3] = a;
                color_i += floats_per_vertex;
            }
            else
            {
                // 4 vertices all in a row
                values[color_i] = colors[color_n++];
                values[color_i+1] = colors[color_n++];
                values[color_i+2] = colors[color_n++];
                values[color_i+3] = colors[color_n++];
                color_i += floats_per_vertex;
                values[color_i] = colors[color_n++];
                values[color_i+1] = colors[color_n++];
                values[color_i+2] = colors[color_n++];
                values[color_i+3] = colors[color_n++];
                color_i += floats_per_vertex;
                values[color_i] = colors[color_n++];
                values[color_i+1] = colors[color_n++];
                values[color_i+2] = colors[color_n++];
                values[color_i+3] = colors[color_n++];
                color_i += floats_per_vertex;
                values[color_i] = colors[color_n++];
                values[color_i+1] = colors[color_n++];
                values[color_i+2] = colors[color_n++];
                values[color_i+3] = colors[color_n++];
                color_i += floats_per_vertex;
            }
        }
    }
	
	current_renderer->impl->BlitBatch(current_renderer, image, target, num_sprites, values, flags | GPU_PASSTHROUGH_ALL);
	free(values);
}

void GPU_TriangleBatch(GPU_Image* image, GPU_Target* target, unsigned short num_vertices, float* values, unsigned int num_indices, unsigned short* indices, GPU_BlitFlagEnum flags)
{
	int src_position_floats_per_vertex;
	int src_texcoord_floats_per_vertex;
	int src_color_floats_per_vertex;

	Uint8 no_positions;
	Uint8 no_texcoords;
	Uint8 no_colors;
	Uint8 pass_texcoords;
	Uint8 pass_colors;

	int src_floats_per_vertex;

	int size;
	float* new_values;

	unsigned int n; // Vertex number iteration variable
	// Source indices
	int pos_n;
	int texcoord_n;
	int color_n;
	// Dest indices
	int vert_i;

	Uint32 tex_w;
	Uint32 tex_h;
	
	Uint8 using_texture = (image != NULL);

    if(!CHECK_RENDERER)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL renderer");
    if(!CHECK_CONTEXT)
        RETURN_ERROR(GPU_ERROR_USER_ERROR, "NULL context");
    
	if(target == NULL)
        RETURN_ERROR(GPU_ERROR_NULL_ARGUMENT, "target");
    
    if(num_vertices == 0)
        return;
    
    // Is it already in the right format?
    if((flags & GPU_PASSTHROUGH_ALL) == GPU_PASSTHROUGH_ALL || values == NULL)
    {
        current_renderer->impl->TriangleBatch(current_renderer, image, target, num_vertices, values, num_indices, indices, flags);
        return;
    }
	
	// Conversion time...
	
	// Convert texcoords and colors for the renderer to use.
	// Condensed: Each vertex has 2 pos, 2 texcoords, 4 color components
	
	// Default values: Each vertex is defined by a position, texcoords, and a color.
	src_position_floats_per_vertex = 2;
	src_texcoord_floats_per_vertex = 2;
	src_color_floats_per_vertex = 4;
	
	no_positions = (Uint8)(flags & GPU_USE_DEFAULT_POSITIONS);
	no_texcoords = (Uint8)(flags & GPU_USE_DEFAULT_SRC_RECTS) || !using_texture;
	no_colors = (Uint8)(flags & GPU_USE_DEFAULT_COLORS);
	pass_texcoords = (Uint8)(flags & GPU_PASSTHROUGH_TEXCOORDS);
	pass_colors = (Uint8)(flags & GPU_PASSTHROUGH_COLORS);
	
	// Vertex position passthrough is ignored (we're not positioning triangles, we're positioning vertices already)
	src_position_floats_per_vertex = 2; // x, y
	if(pass_texcoords)
        src_texcoord_floats_per_vertex = 2; // s, t
	if(pass_colors)
        src_color_floats_per_vertex = 4; // r, g, b, a
	if(no_positions)
        src_position_floats_per_vertex = 0;
	if(no_texcoords)
        src_texcoord_floats_per_vertex = 0;
	if(no_colors)
        src_color_floats_per_vertex = 0;
    
	src_floats_per_vertex = src_position_floats_per_vertex + src_texcoord_floats_per_vertex + src_color_floats_per_vertex;
	
	size = num_vertices*(2 + 2 + 4);
	new_values = (float*)malloc(sizeof(float)*size);
    
	// Source indices
	pos_n = 0;
	texcoord_n = src_position_floats_per_vertex;
	color_n = src_position_floats_per_vertex + src_texcoord_floats_per_vertex;
	// Dest indices
	vert_i = 0;
	
	if(using_texture)
    {
        tex_w = image->base_w;
        tex_h = image->base_h;
    }
	
    for(n = 0; n < num_vertices; n++)
    {
        // 2 floats from position
        if(no_positions)
        {
            new_values[vert_i++] = 0.0f;
            new_values[vert_i++] = 0.0f;
        }
        else
        {
            new_values[vert_i++] = values[pos_n];
            new_values[vert_i++] = values[pos_n+1];
            pos_n += src_floats_per_vertex;
        }
        
        // 2 floats from texcoords
        if(no_texcoords)
        {
            new_values[vert_i++] = 0.0f;
            new_values[vert_i++] = 0.0f;
        }
        else
        {
            if(!pass_texcoords && using_texture)
            {
                new_values[vert_i++] = values[texcoord_n]/tex_w;
                new_values[vert_i++] = values[texcoord_n+1]/tex_h;
                texcoord_n += src_floats_per_vertex;
            }
            else
            {
                new_values[vert_i++] = values[texcoord_n];
                new_values[vert_i++] = values[texcoord_n+1];
            }
        }
        
        if(no_colors)
        {
                new_values[vert_i++] = 1.0f;
                new_values[vert_i++] = 1.0f;
                new_values[vert_i++] = 1.0f;
                new_values[vert_i++] = 1.0f;
        }
        else
        {
            if(!pass_colors)
            {
                new_values[vert_i++] = values[color_n]/255.0f;
                new_values[vert_i++] = values[color_n+1]/255.0f;
                new_values[vert_i++] = values[color_n+2]/255.0f;
                new_values[vert_i++] = values[color_n+3]/255.0f;
                color_n += src_floats_per_vertex;
            }
            else
            {
                new_values[vert_i++] = values[color_n];
                new_values[vert_i++] = values[color_n+1];
                new_values[vert_i++] = values[color_n+2];
                new_values[vert_i++] = values[color_n+3];
                color_n += src_floats_per_vertex;
            }
        }
    }
    
	current_renderer->impl->TriangleBatch(current_renderer, image, target, num_vertices, new_values, num_indices, indices, flags | GPU_PASSTHROUGH_ALL);
	
	free(new_values);
}




void GPU_GenerateMipmaps(GPU_Image* image)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->GenerateMipmaps(current_renderer, image);
}




GPU_Rect GPU_SetClipRect(GPU_Target* target, GPU_Rect rect)
{
	if(target == NULL || current_renderer == NULL || current_renderer->current_context_target == NULL)
	{
		GPU_Rect r = {0,0,0,0};
		return r;
	}
	
	return current_renderer->impl->SetClip(current_renderer, target, (Sint16)rect.x, (Sint16)rect.y, (Uint16)rect.w, (Uint16)rect.h);
}

GPU_Rect GPU_SetClip(GPU_Target* target, Sint16 x, Sint16 y, Uint16 w, Uint16 h)
{
	if(target == NULL || current_renderer == NULL || current_renderer->current_context_target == NULL)
	{
		GPU_Rect r = {0,0,0,0};
		return r;
	}
	
	return current_renderer->impl->SetClip(current_renderer, target, x, y, w, h);
}

void GPU_UnsetClip(GPU_Target* target)
{
	if(target == NULL || current_renderer == NULL || current_renderer->current_context_target == NULL)
        return;
	
	current_renderer->impl->UnsetClip(current_renderer, target);
}




void GPU_SetColor(GPU_Image* image, SDL_Color color)
{
	if(image == NULL)
		return;
	
	image->color = color;
}

void GPU_SetRGB(GPU_Image* image, Uint8 r, Uint8 g, Uint8 b)
{
	SDL_Color c;
	c.r = r;
	c.g = g;
	c.b = b;
	c.a = 255;

	if(image == NULL)
		return;
	
	image->color = c;
}

void GPU_SetRGBA(GPU_Image* image, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	SDL_Color c;
	c.r = r;
	c.g = g;
	c.b = b;
	c.a = a;

	if(image == NULL)
		return;
	
	image->color = c;
}

void GPU_UnsetColor(GPU_Image* image)
{
    SDL_Color c = {255, 255, 255, 255};
	if(image == NULL)
		return;
	
    image->color = c;
}

void GPU_SetTargetColor(GPU_Target* target, SDL_Color color)
{
	if(target == NULL)
		return;
	
    target->use_color = 1;
    target->color = color;
}

void GPU_SetTargetRGB(GPU_Target* target, Uint8 r, Uint8 g, Uint8 b)
{
	SDL_Color c;
	c.r = r;
	c.g = g;
	c.b = b;
	c.a = 255;

	if(target == NULL)
		return;
	
    target->use_color = !(r == 255 && g == 255 && b == 255);
    target->color = c;
}

void GPU_SetTargetRGBA(GPU_Target* target, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	SDL_Color c;
	c.r = r;
	c.g = g;
	c.b = b;
	c.a = a;

	if(target == NULL)
		return;
	
    target->use_color = !(r == 255 && g == 255 && b == 255 && a == 255);
    target->color = c;
}

void GPU_UnsetTargetColor(GPU_Target* target)
{
    SDL_Color c = {255, 255, 255, 255};
	if(target == NULL)
		return;
    
    target->use_color = 0;
    target->color = c;
}

Uint8 GPU_GetBlending(GPU_Image* image)
{
	if(image == NULL)
		return 0;
	
	return image->use_blending;
}


void GPU_SetBlending(GPU_Image* image, Uint8 enable)
{
	if(image == NULL)
		return;
	
	image->use_blending = enable;
}

void GPU_SetShapeBlending(Uint8 enable)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->current_context_target->context->shapes_use_blending = enable;
}


GPU_BlendMode GPU_GetBlendModeFromPreset(GPU_BlendPresetEnum preset)
{
	switch(preset)
	{
    case GPU_BLEND_NORMAL:
        {
            GPU_BlendMode b = {GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_PREMULTIPLIED_ALPHA:
        {
            GPU_BlendMode b = {GPU_FUNC_ONE, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_ONE, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_MULTIPLY:
        {
            GPU_BlendMode b = {GPU_FUNC_DST_COLOR, GPU_FUNC_ZERO, GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_ADD:
        {
            GPU_BlendMode b = {GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE, GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_SUBTRACT:
        // FIXME: Use src alpha for source components?
        {
            GPU_BlendMode b = {GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_EQ_SUBTRACT, GPU_EQ_SUBTRACT};
            return b;
        }
        break;
    case GPU_BLEND_MOD_ALPHA:
        // Don't disturb the colors, but multiply the dest alpha by the src alpha
        {
            GPU_BlendMode b = {GPU_FUNC_ZERO, GPU_FUNC_ONE, GPU_FUNC_ZERO, GPU_FUNC_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_SET_ALPHA:
        // Don't disturb the colors, but set the alpha to the src alpha
        {
            GPU_BlendMode b = {GPU_FUNC_ZERO, GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_FUNC_ZERO, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_SET:
        {
            GPU_BlendMode b = {GPU_FUNC_ONE, GPU_FUNC_ZERO, GPU_FUNC_ONE, GPU_FUNC_ZERO, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_NORMAL_KEEP_ALPHA:
        {
            GPU_BlendMode b = {GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_ZERO, GPU_FUNC_ONE, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    case GPU_BLEND_NORMAL_ADD_ALPHA:
        {
            GPU_BlendMode b = {GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_ONE, GPU_FUNC_ONE, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
    default:
        GPU_PushErrorCode(__func__, GPU_ERROR_USER_ERROR, "Blend preset not supported: %d", preset);
        {
            GPU_BlendMode b = {GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_FUNC_SRC_ALPHA, GPU_FUNC_ONE_MINUS_SRC_ALPHA, GPU_EQ_ADD, GPU_EQ_ADD};
            return b;
        }
        break;
	}
}


void GPU_SetBlendFunction(GPU_Image* image, GPU_BlendFuncEnum source_color, GPU_BlendFuncEnum dest_color, GPU_BlendFuncEnum source_alpha, GPU_BlendFuncEnum dest_alpha)
{
	if(image == NULL)
		return;
	
	image->blend_mode.source_color = source_color;
	image->blend_mode.dest_color = dest_color;
	image->blend_mode.source_alpha = source_alpha;
	image->blend_mode.dest_alpha = dest_alpha;
}

void GPU_SetBlendEquation(GPU_Image* image, GPU_BlendEqEnum color_equation, GPU_BlendEqEnum alpha_equation)
{
	if(image == NULL)
		return;
    
    image->blend_mode.color_equation = color_equation;
    image->blend_mode.alpha_equation = alpha_equation;
}

void GPU_SetBlendMode(GPU_Image* image, GPU_BlendPresetEnum preset)
{
    GPU_BlendMode b;
	if(image == NULL)
		return;
	
	b = GPU_GetBlendModeFromPreset(preset);
    GPU_SetBlendFunction(image, b.source_color, b.dest_color, b.source_alpha, b.dest_alpha);
    GPU_SetBlendEquation(image, b.color_equation, b.alpha_equation);
}

void GPU_SetShapeBlendFunction(GPU_BlendFuncEnum source_color, GPU_BlendFuncEnum dest_color, GPU_BlendFuncEnum source_alpha, GPU_BlendFuncEnum dest_alpha)
{
    GPU_Context* context;
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	context = current_renderer->current_context_target->context;
	
	context->shapes_blend_mode.source_color = source_color;
	context->shapes_blend_mode.dest_color = dest_color;
	context->shapes_blend_mode.source_alpha = source_alpha;
	context->shapes_blend_mode.dest_alpha = dest_alpha;
}

void GPU_SetShapeBlendEquation(GPU_BlendEqEnum color_equation, GPU_BlendEqEnum alpha_equation)
{
    GPU_Context* context;
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
    
	context = current_renderer->current_context_target->context;
    
    context->shapes_blend_mode.color_equation = color_equation;
    context->shapes_blend_mode.alpha_equation = alpha_equation;
}

void GPU_SetShapeBlendMode(GPU_BlendPresetEnum preset)
{
    GPU_BlendMode b;
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	b = GPU_GetBlendModeFromPreset(preset);
    GPU_SetShapeBlendFunction(b.source_color, b.dest_color, b.source_alpha, b.dest_alpha);
    GPU_SetShapeBlendEquation(b.color_equation, b.alpha_equation);
}

void GPU_SetImageFilter(GPU_Image* image, GPU_FilterEnum filter)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	if(image == NULL)
		return;
	
	current_renderer->impl->SetImageFilter(current_renderer, image, filter);
}

GPU_SnapEnum GPU_GetSnapMode(GPU_Image* image)
{
	if(image == NULL)
		return 0;
	
	return image->snap_mode;
}

void GPU_SetSnapMode(GPU_Image* image, GPU_SnapEnum mode)
{
	if(image == NULL)
		return;
	
	image->snap_mode = mode;
}

void GPU_SetWrapMode(GPU_Image* image, GPU_WrapEnum wrap_mode_x, GPU_WrapEnum wrap_mode_y)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	if(image == NULL)
		return;
	
	current_renderer->impl->SetWrapMode(current_renderer, image, wrap_mode_x, wrap_mode_y);
}


SDL_Color GPU_GetPixel(GPU_Target* target, Sint16 x, Sint16 y)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
	{
		SDL_Color c = {0,0,0,0};
		return c;
	}
	
	return current_renderer->impl->GetPixel(current_renderer, target, x, y);
}







void GPU_Clear(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->ClearRGBA(current_renderer, target, 0, 0, 0, 0);
}

void GPU_ClearColor(GPU_Target* target, SDL_Color color)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
    current_renderer->impl->ClearRGBA(current_renderer, target, color.r, color.g, color.b, GET_ALPHA(color));
}

void GPU_ClearRGB(GPU_Target* target, Uint8 r, Uint8 g, Uint8 b)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->ClearRGBA(current_renderer, target, r, g, b, 255);
}

void GPU_ClearRGBA(GPU_Target* target, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->ClearRGBA(current_renderer, target, r, g, b, a);
}

void GPU_FlushBlitBuffer(void)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->FlushBlitBuffer(current_renderer);
}

void GPU_Flip(GPU_Target* target)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->Flip(current_renderer, target);
}





// Shader API


Uint32 GPU_CompileShader_RW(GPU_ShaderEnum shader_type, SDL_RWops* shader_source)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->CompileShader_RW(current_renderer, shader_type, shader_source);
}

Uint32 GPU_LoadShader(GPU_ShaderEnum shader_type, const char* filename)
{
	SDL_RWops* rwops;
	Uint32 result;

    if(filename == NULL)
    {
        GPU_PushErrorCode(__func__, GPU_ERROR_NULL_ARGUMENT, "filename");
        return 0;
    }
    rwops = SDL_RWFromFile(filename, "r");
    if(rwops == NULL)
    {
        GPU_PushErrorCode(__func__, GPU_ERROR_FILE_NOT_FOUND, "%s", filename);
        return 0;
    }
    result = GPU_CompileShader_RW(shader_type, rwops);
    SDL_RWclose(rwops);
    return result;
}

Uint32 GPU_CompileShader(GPU_ShaderEnum shader_type, const char* shader_source)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->CompileShader(current_renderer, shader_type, shader_source);
}

Uint8 GPU_LinkShaderProgram(Uint32 program_object)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->LinkShaderProgram(current_renderer, program_object);
}

Uint32 GPU_CreateShaderProgram(void)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
    
	return current_renderer->impl->CreateShaderProgram(current_renderer);
}

Uint32 GPU_LinkShaders(Uint32 shader_object1, Uint32 shader_object2)
{
    Uint32 p;
    
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
    if((current_renderer->enabled_features & GPU_FEATURE_BASIC_SHADERS) != GPU_FEATURE_BASIC_SHADERS)
        return 0;
    
    p = current_renderer->impl->CreateShaderProgram(current_renderer);

	current_renderer->impl->AttachShader(current_renderer, p, shader_object1);
	current_renderer->impl->AttachShader(current_renderer, p, shader_object2);
	
	if(current_renderer->impl->LinkShaderProgram(current_renderer, p))
        return p;
    
    current_renderer->impl->FreeShaderProgram(current_renderer, p);
    return 0;
}

void GPU_FreeShader(Uint32 shader_object)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->FreeShader(current_renderer, shader_object);
}

void GPU_FreeShaderProgram(Uint32 program_object)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->FreeShaderProgram(current_renderer, program_object);
}

void GPU_AttachShader(Uint32 program_object, Uint32 shader_object)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->AttachShader(current_renderer, program_object, shader_object);
}

void GPU_DetachShader(Uint32 program_object, Uint32 shader_object)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->DetachShader(current_renderer, program_object, shader_object);
}

Uint8 GPU_IsDefaultShaderProgram(Uint32 program_object)
{
    GPU_Context* context;
    
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
    
    context = current_renderer->current_context_target->context;
    return (program_object == context->default_textured_shader_program || program_object == context->default_untextured_shader_program);
}

void GPU_ActivateShaderProgram(Uint32 program_object, GPU_ShaderBlock* block)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->ActivateShaderProgram(current_renderer, program_object, block);
}

void GPU_DeactivateShaderProgram(void)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->DeactivateShaderProgram(current_renderer);
}

const char* GPU_GetShaderMessage(void)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return NULL;
	
	return current_renderer->impl->GetShaderMessage(current_renderer);
}

int GPU_GetAttributeLocation(Uint32 program_object, const char* attrib_name)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->GetAttributeLocation(current_renderer, program_object, attrib_name);
}

GPU_AttributeFormat GPU_MakeAttributeFormat(int num_elems_per_vertex, GPU_TypeEnum type, Uint8 normalize, int stride_bytes, int offset_bytes)
{
	GPU_AttributeFormat f;
	f.is_per_sprite = 0;
	f.num_elems_per_value = num_elems_per_vertex;
	f.type = type;
	f.normalize = normalize;
	f.stride_bytes = stride_bytes;
	f.offset_bytes = offset_bytes;
    return f;
}

GPU_Attribute GPU_MakeAttribute(int location, void* values, GPU_AttributeFormat format)
{
	GPU_Attribute a;
	a.location = location;
	a.values = values;
	a.format = format;
    return a;
}

int GPU_GetUniformLocation(Uint32 program_object, const char* uniform_name)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return 0;
	
	return current_renderer->impl->GetUniformLocation(current_renderer, program_object, uniform_name);
}

GPU_ShaderBlock GPU_LoadShaderBlock(Uint32 program_object, const char* position_name, const char* texcoord_name, const char* color_name, const char* modelViewMatrix_name)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
    {
        GPU_ShaderBlock b;
        b.position_loc = -1;
        b.texcoord_loc = -1;
        b.color_loc = -1;
        b.modelViewProjection_loc = -1;
		return b;
    }
	
	return current_renderer->impl->LoadShaderBlock(current_renderer, program_object, position_name, texcoord_name, color_name, modelViewMatrix_name);
}

void GPU_SetShaderBlock(GPU_ShaderBlock block)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetShaderBlock(current_renderer, block);
}

void GPU_SetShaderImage(GPU_Image* image, int location, int image_unit)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetShaderImage(current_renderer, image, location, image_unit);
}

void GPU_GetUniformiv(Uint32 program_object, int location, int* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->GetUniformiv(current_renderer, program_object, location, values);
}

void GPU_SetUniformi(int location, int value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformi(current_renderer, location, value);
}

void GPU_SetUniformiv(int location, int num_elements_per_value, int num_values, int* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformiv(current_renderer, location, num_elements_per_value, num_values, values);
}


void GPU_GetUniformuiv(Uint32 program_object, int location, unsigned int* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->GetUniformuiv(current_renderer, program_object, location, values);
}

void GPU_SetUniformui(int location, unsigned int value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformui(current_renderer, location, value);
}

void GPU_SetUniformuiv(int location, int num_elements_per_value, int num_values, unsigned int* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformuiv(current_renderer, location, num_elements_per_value, num_values, values);
}


void GPU_GetUniformfv(Uint32 program_object, int location, float* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->GetUniformfv(current_renderer, program_object, location, values);
}

void GPU_SetUniformf(int location, float value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformf(current_renderer, location, value);
}

void GPU_SetUniformfv(int location, int num_elements_per_value, int num_values, float* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformfv(current_renderer, location, num_elements_per_value, num_values, values);
}

// Same as GPU_GetUniformfv()
void GPU_GetUniformMatrixfv(Uint32 program_object, int location, float* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->GetUniformfv(current_renderer, program_object, location, values);
}

void GPU_SetUniformMatrixfv(int location, int num_matrices, int num_rows, int num_columns, Uint8 transpose, float* values)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetUniformMatrixfv(current_renderer, location, num_matrices, num_rows, num_columns, transpose, values);
}


void GPU_SetAttributef(int location, float value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributef(current_renderer, location, value);
}

void GPU_SetAttributei(int location, int value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributei(current_renderer, location, value);
}

void GPU_SetAttributeui(int location, unsigned int value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributeui(current_renderer, location, value);
}

void GPU_SetAttributefv(int location, int num_elements, float* value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributefv(current_renderer, location, num_elements, value);
}

void GPU_SetAttributeiv(int location, int num_elements, int* value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributeiv(current_renderer, location, num_elements, value);
}

void GPU_SetAttributeuiv(int location, int num_elements, unsigned int* value)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributeuiv(current_renderer, location, num_elements, value);
}

void GPU_SetAttributeSource(int num_values, GPU_Attribute source)
{
	if(current_renderer == NULL || current_renderer->current_context_target == NULL)
		return;
	
	current_renderer->impl->SetAttributeSource(current_renderer, num_values, source);
}




// GPU_strcasecmp()
// A portable strcasecmp() from UC Berkeley
/*
 * Copyright (c) 1987 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of California at Berkeley. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific written prior permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

/*
 * This array is designed for mapping upper and lower case letter
 * together for a case independent comparison.  The mappings are
 * based upon ascii character sequences.
 */
static const unsigned char caseless_charmap[] = {
	'\000', '\001', '\002', '\003', '\004', '\005', '\006', '\007',
	'\010', '\011', '\012', '\013', '\014', '\015', '\016', '\017',
	'\020', '\021', '\022', '\023', '\024', '\025', '\026', '\027',
	'\030', '\031', '\032', '\033', '\034', '\035', '\036', '\037',
	'\040', '\041', '\042', '\043', '\044', '\045', '\046', '\047',
	'\050', '\051', '\052', '\053', '\054', '\055', '\056', '\057',
	'\060', '\061', '\062', '\063', '\064', '\065', '\066', '\067',
	'\070', '\071', '\072', '\073', '\074', '\075', '\076', '\077',
	'\100', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
	'\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
	'\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
	'\170', '\171', '\172', '\133', '\134', '\135', '\136', '\137',
	'\140', '\141', '\142', '\143', '\144', '\145', '\146', '\147',
	'\150', '\151', '\152', '\153', '\154', '\155', '\156', '\157',
	'\160', '\161', '\162', '\163', '\164', '\165', '\166', '\167',
	'\170', '\171', '\172', '\173', '\174', '\175', '\176', '\177',
	'\200', '\201', '\202', '\203', '\204', '\205', '\206', '\207',
	'\210', '\211', '\212', '\213', '\214', '\215', '\216', '\217',
	'\220', '\221', '\222', '\223', '\224', '\225', '\226', '\227',
	'\230', '\231', '\232', '\233', '\234', '\235', '\236', '\237',
	'\240', '\241', '\242', '\243', '\244', '\245', '\246', '\247',
	'\250', '\251', '\252', '\253', '\254', '\255', '\256', '\257',
	'\260', '\261', '\262', '\263', '\264', '\265', '\266', '\267',
	'\270', '\271', '\272', '\273', '\274', '\275', '\276', '\277',
	'\300', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
	'\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
	'\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
	'\370', '\371', '\372', '\333', '\334', '\335', '\336', '\337',
	'\340', '\341', '\342', '\343', '\344', '\345', '\346', '\347',
	'\350', '\351', '\352', '\353', '\354', '\355', '\356', '\357',
	'\360', '\361', '\362', '\363', '\364', '\365', '\366', '\367',
	'\370', '\371', '\372', '\373', '\374', '\375', '\376', '\377',
};

int GPU_strcasecmp(const char* s1, const char* s2)
{
    unsigned char u1, u2;

    for (;;)
    {
        u1 = (unsigned char) *s1++;
        u2 = (unsigned char) *s2++;
        if (caseless_charmap[u1] != caseless_charmap[u2])
            return caseless_charmap[u1] - caseless_charmap[u2];
        if (u1 == '\0')
            return 0;
    }
    return 0;
}


#ifdef _MSC_VER
	#pragma warning(pop) 
#endif

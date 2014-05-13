#include "SDL.h"
#include "SDL_gpu.h"
#include "common.h"


GPU_ShaderBlock load_shaders(Uint32* v, Uint32* f, Uint32* p)
{
    GPU_Renderer* renderer = GPU_GetCurrentRenderer();
    const char* vertex_shader_file;
    const char* fragment_shader_file;
    if(renderer->shader_version < 130)
    {
        vertex_shader_file = "shader/test.vert";
        fragment_shader_file = "shader/test.frag";
    }
    else
    {
        vertex_shader_file = "shader/test3.vert";
        fragment_shader_file = "shader/test3.frag";
    }
    
    *v = GPU_LoadShader(GPU_VERTEX_SHADER, vertex_shader_file);
    
    if(!*v)
        GPU_LogError("Failed to load vertex shader: %s\n", GPU_GetShaderMessage());
    
    *f = GPU_LoadShader(GPU_FRAGMENT_SHADER, fragment_shader_file);
    
    if(!*f)
        GPU_LogError("Failed to load fragment shader: %s\n", GPU_GetShaderMessage());
    
    *p = GPU_LinkShaders(*v, *f);
    
    if(!*p)
    {
		GPU_ShaderBlock b = {-1, -1, -1, -1};
        GPU_LogError("Failed to link shader program: %s\n", GPU_GetShaderMessage());
        return b;
    }
    
	{
		GPU_ShaderBlock block = GPU_LoadShaderBlock(*p, "gpu_Vertex", "gpu_TexCoord", "gpu_Color", "modelViewProjection");
		GPU_ActivateShaderProgram(*p, &block);

		return block;
	}
}

void free_shaders(Uint32 v, Uint32 f, Uint32 p)
{
    GPU_FreeShader(v);
    GPU_FreeShader(f);
    GPU_FreeShaderProgram(p);
}

int main(int argc, char* argv[])
{
	GPU_Target* screen;

	printRenderers();
	
	screen = GPU_Init(800, 600, GPU_DEFAULT_INIT_FLAGS);
	if(screen == NULL)
		return -1;
	
	printCurrentRenderer();
	
	{
		Uint32 startTime;
		long frameCount;
		Uint8 done;
		SDL_Event event;
		
		GPU_Image* image;
        Uint32 v, f, p;
        GPU_ShaderBlock block;
        int uloc;
        int timeloc;
        float dt;
        #define MAX_SPRITES 50
        int numSprites;
        float x[MAX_SPRITES];
        float y[MAX_SPRITES];
        float velx[MAX_SPRITES];
        float vely[MAX_SPRITES];
        int i;
        
        image = GPU_LoadImage("data/test.bmp");
        if(image == NULL)
            return -1;
        
        block = load_shaders(&v, &f, &p);
        uloc = GPU_GetUniformLocation(p, "tex");
        GPU_SetUniformi(uloc, 0);
        timeloc = GPU_GetUniformLocation(p, "time");
        
        dt = 0.010f;
        
        startTime = SDL_GetTicks();
        frameCount = 0;
        
        numSprites = 1;
        
        for(i = 0; i < MAX_SPRITES; i++)
        {
            x[i] = rand()%screen->w;
            y[i] = rand()%screen->h;
            velx[i] = 10 + rand()%screen->w/10;
            vely[i] = 10 + rand()%screen->h/10;
        }
        
        
        done = 0;
        while(!done)
        {
            while(SDL_PollEvent(&event))
            {
                if(event.type == SDL_QUIT)
                    done = 1;
                else if(event.type == SDL_KEYDOWN)
                {
                    if(event.key.keysym.sym == SDLK_ESCAPE)
                        done = 1;
                    else if(event.key.keysym.sym == SDLK_EQUALS || event.key.keysym.sym == SDLK_PLUS)
                    {
                        if(numSprites < MAX_SPRITES)
                            numSprites++;
                    }
                    else if(event.key.keysym.sym == SDLK_MINUS)
                    {
                        if(numSprites > 0)
                            numSprites--;
                    }
                    else if(event.key.keysym.sym == SDLK_SPACE)
                    {
                        if(GPU_IsDefaultShaderProgram(GPU_GetCurrentShaderProgram()))
                        {
                            GPU_ActivateShaderProgram(p, &block);
                            uloc = GPU_GetUniformLocation(p, "tex");
                            GPU_SetUniformi(uloc, 0);
                            timeloc = GPU_GetUniformLocation(p, "time");
                        }
                        else
                            GPU_ActivateShaderProgram(0, NULL);
                    }
                }
            }
            
            for(i = 0; i < numSprites; i++)
            {
                x[i] += velx[i]*dt;
                y[i] += vely[i]*dt;
                if(x[i] < 0)
                {
                    x[i] = 0;
                    velx[i] = -velx[i];
                }
                else if(x[i]> screen->w)
                {
                    x[i] = screen->w;
                    velx[i] = -velx[i];
                }
                
                if(y[i] < 0)
                {
                    y[i] = 0;
                    vely[i] = -vely[i];
                }
                else if(y[i]> screen->h)
                {
                    y[i] = screen->h;
                    vely[i] = -vely[i];
                }
            }
            
            GPU_SetUniformf(timeloc, SDL_GetTicks()/1000.0f);
            
            GPU_Clear(screen);
            
            for(i = 0; i < numSprites; i++)
            {
                GPU_Blit(image, NULL, screen, x[i], y[i]);
            }
            
            GPU_Flip(screen);
            
            frameCount++;
            if(frameCount%500 == 0)
                printf("Average FPS: %.2f\n", 1000.0f*frameCount/(SDL_GetTicks() - startTime));
        }
        
        printf("Average FPS: %.2f\n", 1000.0f*frameCount/(SDL_GetTicks() - startTime));
        
        GPU_FreeImage(image);
        
        free_shaders(v, f, p);
	}
	
	GPU_Quit();
	
	return 0;
}



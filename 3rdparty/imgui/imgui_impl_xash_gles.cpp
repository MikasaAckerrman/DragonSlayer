/*
imgui_impl_xash_gles.cpp - Dear ImGui backend for Xash3D (OpenGL ES 2.0)
Copyright (C) 2026 Slayer3D contributors

Minimal GLES2 renderer for ImGui. Uses raw gl* calls.
Does NOT depend on GLEW, GLAD, or any desktop GL loader.
*/

#include "imgui_impl_xash_gles.h"
#include "imgui.h"

#include <stdint.h>

#ifdef __ANDROID__
#include <GLES2/gl2.h>
#else
// Desktop fallback: try GLES2 headers (available when targeting GLES on desktop)
// On systems without GLES2 headers, supply them via include path from the build system.
#if defined(__has_include)
#if __has_include(<GLES2/gl2.h>)
#include <GLES2/gl2.h>
#else
#include <GL/gl.h>
#endif
#else
#include <GLES2/gl2.h>
#endif
#endif

// Shader sources (GLSL ES 1.00)
static const char *g_VertexShaderSrc =
	"precision mediump float;\n"
	"uniform mat4 u_ProjMtx;\n"
	"attribute vec2 a_Position;\n"
	"attribute vec2 a_TexCoord;\n"
	"attribute vec4 a_Color;\n"
	"varying vec2 v_TexCoord;\n"
	"varying vec4 v_Color;\n"
	"void main() {\n"
	"    v_TexCoord = a_TexCoord;\n"
	"    v_Color = a_Color;\n"
	"    gl_Position = u_ProjMtx * vec4(a_Position, 0.0, 1.0);\n"
	"}\n";

static const char *g_FragmentShaderSrc =
	"precision mediump float;\n"
	"uniform sampler2D u_Texture;\n"
	"varying vec2 v_TexCoord;\n"
	"varying vec4 v_Color;\n"
	"void main() {\n"
	"    gl_FragColor = v_Color * texture2D(u_Texture, v_TexCoord);\n"
	"}\n";

// Backend state
static GLuint g_ShaderProgram = 0;
static GLint  g_AttribLocPosition = -1;
static GLint  g_AttribLocTexCoord = -1;
static GLint  g_AttribLocColor = -1;
static GLint  g_UniformLocProjMtx = -1;
static GLint  g_UniformLocTexture = -1;
static GLuint g_FontTexture = 0;
static GLuint g_VboHandle = 0;
static GLuint g_ElementsHandle = 0;

static GLuint CompileShader( GLenum type, const char *source )
{
	GLuint shader = glCreateShader( type );
	glShaderSource( shader, 1, &source, 0 );
	glCompileShader( shader );

	GLint status = 0;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if( status == GL_FALSE )
	{
		GLchar info[512];
		glGetShaderInfoLog( shader, 512, 0, info );
		(void)info;
		glDeleteShader( shader );
		return 0;
	}
	return shader;
}

static bool CreateDeviceObjects( void )
{
	GLuint vs = CompileShader( GL_VERTEX_SHADER, g_VertexShaderSrc );
	GLuint fs = CompileShader( GL_FRAGMENT_SHADER, g_FragmentShaderSrc );

	if( !vs || !fs )
	{
		if( vs ) glDeleteShader( vs );
		if( fs ) glDeleteShader( fs );
		return false;
	}

	g_ShaderProgram = glCreateProgram();
	glAttachShader( g_ShaderProgram, vs );
	glAttachShader( g_ShaderProgram, fs );
	glLinkProgram( g_ShaderProgram );

	GLint status = 0;
	glGetProgramiv( g_ShaderProgram, GL_LINK_STATUS, &status );
	if( status == GL_FALSE )
	{
		glDeleteProgram( g_ShaderProgram );
		g_ShaderProgram = 0;
		glDeleteShader( vs );
		glDeleteShader( fs );
		return false;
	}

	glDeleteShader( vs );
	glDeleteShader( fs );

	g_AttribLocPosition = glGetAttribLocation( g_ShaderProgram, "a_Position" );
	g_AttribLocTexCoord = glGetAttribLocation( g_ShaderProgram, "a_TexCoord" );
	g_AttribLocColor    = glGetAttribLocation( g_ShaderProgram, "a_Color" );
	g_UniformLocProjMtx = glGetUniformLocation( g_ShaderProgram, "u_ProjMtx" );
	g_UniformLocTexture = glGetUniformLocation( g_ShaderProgram, "u_Texture" );

	glGenBuffers( 1, &g_VboHandle );
	glGenBuffers( 1, &g_ElementsHandle );

	return true;
}

static void CreateFontTexture( void )
{
	ImGuiIO &io = ImGui::GetIO();
	unsigned char *pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32( &pixels, &width, &height );

	glGenTextures( 1, &g_FontTexture );
	glBindTexture( GL_TEXTURE_2D, g_FontTexture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );

	io.Fonts->SetTexID( (ImTextureID)(intptr_t)g_FontTexture );
}

bool ImGui_ImplXashGLES_Init( void )
{
	if( !CreateDeviceObjects() )
		return false;

	CreateFontTexture();
	return true;
}

void ImGui_ImplXashGLES_Shutdown( void )
{
	if( g_FontTexture )
	{
		glDeleteTextures( 1, &g_FontTexture );
		g_FontTexture = 0;
	}
	if( g_VboHandle )
	{
		glDeleteBuffers( 1, &g_VboHandle );
		g_VboHandle = 0;
	}
	if( g_ElementsHandle )
	{
		glDeleteBuffers( 1, &g_ElementsHandle );
		g_ElementsHandle = 0;
	}
	if( g_ShaderProgram )
	{
		glDeleteProgram( g_ShaderProgram );
		g_ShaderProgram = 0;
	}
}

void ImGui_ImplXashGLES_NewFrame( void )
{
	// Nothing to do per-frame for the GLES backend.
	// Display size is set externally via ImGuiIO.
}

void ImGui_ImplXashGLES_RenderDrawData( ImDrawData *draw_data )
{
	if( !draw_data || draw_data->CmdListsCount == 0 )
		return;

	float fb_width  = draw_data->DisplaySize.x * draw_data->FramebufferScale.x;
	float fb_height = draw_data->DisplaySize.y * draw_data->FramebufferScale.y;
	if( fb_width <= 0.0f || fb_height <= 0.0f )
		return;

	// Save GL state
	GLint last_program; glGetIntegerv( GL_CURRENT_PROGRAM, &last_program );
	GLint last_texture; glGetIntegerv( GL_TEXTURE_BINDING_2D, &last_texture );
	GLint last_array_buffer; glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &last_array_buffer );
	GLint last_element_array_buffer; glGetIntegerv( GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer );
	GLint last_viewport[4]; glGetIntegerv( GL_VIEWPORT, last_viewport );
	GLint last_scissor_box[4]; glGetIntegerv( GL_SCISSOR_BOX, last_scissor_box );
	GLboolean last_enable_blend = glIsEnabled( GL_BLEND );
	GLboolean last_enable_cull_face = glIsEnabled( GL_CULL_FACE );
	GLboolean last_enable_depth_test = glIsEnabled( GL_DEPTH_TEST );
	GLboolean last_enable_scissor_test = glIsEnabled( GL_SCISSOR_TEST );
	GLint last_blend_src_rgb; glGetIntegerv( GL_BLEND_SRC_RGB, &last_blend_src_rgb );
	GLint last_blend_dst_rgb; glGetIntegerv( GL_BLEND_DST_RGB, &last_blend_dst_rgb );
	GLint last_blend_src_alpha; glGetIntegerv( GL_BLEND_SRC_ALPHA, &last_blend_src_alpha );
	GLint last_blend_dst_alpha; glGetIntegerv( GL_BLEND_DST_ALPHA, &last_blend_dst_alpha );
	GLint last_blend_equation_rgb; glGetIntegerv( GL_BLEND_EQUATION_RGB, &last_blend_equation_rgb );
	GLint last_blend_equation_alpha; glGetIntegerv( GL_BLEND_EQUATION_ALPHA, &last_blend_equation_alpha );

	// Setup render state
	glEnable( GL_BLEND );
	glBlendEquation( GL_FUNC_ADD );
	glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	glDisable( GL_CULL_FACE );
	glDisable( GL_DEPTH_TEST );
	glEnable( GL_SCISSOR_TEST );
	glActiveTexture( GL_TEXTURE0 );

	glViewport( 0, 0, (GLsizei)fb_width, (GLsizei)fb_height );

	// Orthographic projection matrix
	float L = draw_data->DisplayPos.x;
	float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
	float T = draw_data->DisplayPos.y;
	float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
	float ortho[4][4] =
	{
		{ 2.0f / ( R - L ),    0.0f,                0.0f, 0.0f },
		{ 0.0f,                2.0f / ( T - B ),    0.0f, 0.0f },
		{ 0.0f,                0.0f,               -1.0f, 0.0f },
		{ ( R + L ) / ( L - R ), ( T + B ) / ( B - T ), 0.0f, 1.0f },
	};

	glUseProgram( g_ShaderProgram );
	glUniform1i( g_UniformLocTexture, 0 );
	glUniformMatrix4fv( g_UniformLocProjMtx, 1, GL_FALSE, &ortho[0][0] );

	glBindBuffer( GL_ARRAY_BUFFER, g_VboHandle );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, g_ElementsHandle );

	glEnableVertexAttribArray( (GLuint)g_AttribLocPosition );
	glEnableVertexAttribArray( (GLuint)g_AttribLocTexCoord );
	glEnableVertexAttribArray( (GLuint)g_AttribLocColor );

#define OFFSETOF(TYPE, ELEMENT) ((size_t)&(((TYPE *)0)->ELEMENT))
	glVertexAttribPointer( (GLuint)g_AttribLocPosition, 2, GL_FLOAT, GL_FALSE, sizeof( ImDrawVert ), (void *)OFFSETOF( ImDrawVert, pos ) );
	glVertexAttribPointer( (GLuint)g_AttribLocTexCoord, 2, GL_FLOAT, GL_FALSE, sizeof( ImDrawVert ), (void *)OFFSETOF( ImDrawVert, uv ) );
	glVertexAttribPointer( (GLuint)g_AttribLocColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( ImDrawVert ), (void *)OFFSETOF( ImDrawVert, col ) );
#undef OFFSETOF

	ImVec2 clip_off = draw_data->DisplayPos;
	ImVec2 clip_scale = draw_data->FramebufferScale;

	for( int n = 0; n < draw_data->CmdListsCount; n++ )
	{
		const ImDrawList *cmd_list = draw_data->CmdLists[n];

		glBufferData( GL_ARRAY_BUFFER,
			(GLsizeiptr)cmd_list->VtxBuffer.Size * (int)sizeof( ImDrawVert ),
			(const void *)cmd_list->VtxBuffer.Data, GL_STREAM_DRAW );
		glBufferData( GL_ELEMENT_ARRAY_BUFFER,
			(GLsizeiptr)cmd_list->IdxBuffer.Size * (int)sizeof( ImDrawIdx ),
			(const void *)cmd_list->IdxBuffer.Data, GL_STREAM_DRAW );

		for( int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++ )
		{
			const ImDrawCmd *pcmd = &cmd_list->CmdBuffer[cmd_i];

			if( pcmd->UserCallback != NULL )
			{
				pcmd->UserCallback( cmd_list, pcmd );
			}
			else
			{
				ImVec2 clip_min( ( pcmd->ClipRect.x - clip_off.x ) * clip_scale.x,
				                 ( pcmd->ClipRect.y - clip_off.y ) * clip_scale.y );
				ImVec2 clip_max( ( pcmd->ClipRect.z - clip_off.x ) * clip_scale.x,
				                 ( pcmd->ClipRect.w - clip_off.y ) * clip_scale.y );

				if( clip_max.x <= clip_min.x || clip_max.y <= clip_min.y )
					continue;

				glScissor( (int)clip_min.x,
				           (int)( fb_height - clip_max.y ),
				           (int)( clip_max.x - clip_min.x ),
				           (int)( clip_max.y - clip_min.y ) );

				glBindTexture( GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->GetTexID() );
				glDrawElements( GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
					sizeof( ImDrawIdx ) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
					(void *)(intptr_t)( pcmd->IdxOffset * sizeof( ImDrawIdx ) ) );
			}
		}
	}

	// Restore GL state
	glUseProgram( (GLuint)last_program );
	glBindTexture( GL_TEXTURE_2D, (GLuint)last_texture );
	glBindBuffer( GL_ARRAY_BUFFER, (GLuint)last_array_buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, (GLuint)last_element_array_buffer );
	glBlendEquationSeparate( (GLenum)last_blend_equation_rgb, (GLenum)last_blend_equation_alpha );
	glBlendFuncSeparate( (GLenum)last_blend_src_rgb, (GLenum)last_blend_dst_rgb, (GLenum)last_blend_src_alpha, (GLenum)last_blend_dst_alpha );
	if( last_enable_blend ) glEnable( GL_BLEND ); else glDisable( GL_BLEND );
	if( last_enable_cull_face ) glEnable( GL_CULL_FACE ); else glDisable( GL_CULL_FACE );
	if( last_enable_depth_test ) glEnable( GL_DEPTH_TEST ); else glDisable( GL_DEPTH_TEST );
	if( last_enable_scissor_test ) glEnable( GL_SCISSOR_TEST ); else glDisable( GL_SCISSOR_TEST );
	glViewport( last_viewport[0], last_viewport[1], (GLsizei)last_viewport[2], (GLsizei)last_viewport[3] );
	glScissor( last_scissor_box[0], last_scissor_box[1], (GLsizei)last_scissor_box[2], (GLsizei)last_scissor_box[3] );
}

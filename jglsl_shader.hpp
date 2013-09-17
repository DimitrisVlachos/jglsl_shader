/*
	A GLSL shader/wrapper implementation with the ability to parse automatically uniform/attribute variables.
	As a bonus it also supports structure data types. 
	(Ie does more than glGetActiveUniform )

	Author  : Dimitris Vlachos (DimitrisV22@gmail.com @https://github.com/DimitrisVlachos)
	Licence : MIT 
*/

#ifndef _jglsl_shader_c_
#define _jglsl_shader_c_


/*Place your includes here*/
#if 0
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <iostream>
#include <vector>
#include <map>

/*Define it to remove all glUniform##() macros*/
#undef JGLSL_NO_GLUNIFORM_MACROS
/*
	Example usage :

	jglsl_shader_c* shader = new jglsl_shader_c();
	
    char* v_shader = read_nullterm_text_file("shaders/test.vert");
    char* f_shader = read_nullterm_text_file("shaders/test.frag");
    shader->load(GL_FRAGMENT_SHADER,f_shader,strlen(f_shader));
    shader->load(GL_VERTEX_SHADER,v_shader,strlen(v_shader));
    delete v_shader; delete f_shader;


	shader->finalize(); 	//Will link shaders & resolve all symbol addresses

	if (shader->get_log())
		printf("Log : %s\n",shader->get_log());

	uint32_t some_attr_location = shader->get_attribute("a_var");
	uint32_t some_uni_location = shader->get_uniform("u_var");


	//Structure access :
	glsl shader :
	struct var3_t { int a2; int b2; int c2; }
	struct var_t { int a; int b; int c; var3_t f; }
	struct var2_t {var_t other; int d; int e; }

	uniform var2_t test;
	...


	To access test.other.a; do :
	uint32_t some_uni_location = shader->get_uniform("test.other.a");

	To access test.other.f.a2; do :
	uint32_t some_uni_location = shader->get_uniform("test.other.f.a2");

	To access test.e; do :
	uint32_t some_uni_location = shader->get_uniform("test.e");

	To modify location by name do :
	shader->u_s32("test.other.f.a2",0);

	To modify location by index do :
	shader->u_s32(some_uni_location,0);
*/

class jglsl_shader_c {
	private:
	std::map<std::string,uint32_t> m_attributes; 
	std::map<std::string,uint32_t> m_uniforms;
	std::vector<std::string> m_builtin_types;			//Standard builtin types (scanned by match-case)
	std::vector<std::string> m_complex_builtin_types;	//Special builtin types (scanned by pattern)
	std::vector<std::string> m_pending_uniforms;
	std::vector<std::string> m_pending_attributes;
	std::vector<GLuint> m_shaders;
	std::vector<char> m_log_buffer;
	GLuint m_program;

	//The parser can be improved with pointer math ,  although its highly unlikely to cause a bottleneck
	//Note:Parsing code has been defined as static to avoid generating all that garbage code within the class
	static uint32_t skip_ws(const char* code,const uint32_t offs,const uint32_t len) {
		uint32_t ptr = offs;
		bool done = false;

		while ( (!done) && (ptr < len) ) {
			done = true;

			//Whitespace first
			while ((ptr < len) && isspace(code[ptr]))
				++ptr;

			//Skip comments
			if (((ptr+1) < len) && (code[ptr] == '/')) {
				if ((code[ptr+1] == '/')) {
					++ptr;
					while ((ptr < len) && ((code[ptr] != '\n') || (code[ptr] != '\r')))
						++ptr;
				} else if ((code[ptr+1] == '*')) { 
					ptr += 2;
					while ((ptr+1 < len) && ( (code[ptr] != '*') && (code[ptr+1] != '/')))
						++ptr;
					if ((ptr+1 < len) && ( (code[ptr] == '*') && (code[ptr+1] == '/')))
						ptr += 2;
				}

				done = false;
			}
 		}

		return ptr;
	}

	static uint32_t next_tok(std::string& tok,const char* code,const uint32_t offs,const uint32_t len) {
		uint32_t ptr = offs;
		tok.clear();

		while ((ptr < len) && (!isspace(code[ptr])) && (code[ptr] != ';') && (code[ptr] != '{') && (code[ptr] != '}')
			 && (code[ptr] != ',')  && (code[ptr] != '/') && (code[ptr] != '[') )
			tok += code[ptr++];

		return ptr;
	}

	static bool is_builtin_type(const std::string& s,
					const std::vector<std::string>& base_types,
					const std::vector<std::string>& complex_types) {
		for (uint32_t i = 0,j = base_types.size();i < j;++i) {
			if (s == base_types[i])
				return true;
		}

		for (uint32_t i = 0,j = complex_types.size();i < j;++i) {
			if (s.find(complex_types[i]) != std::string::npos)
				return true;
		}

		return false;
	}
	
	static std::vector<std::string>* get_struct_field(const std::string& name,std::map<std::string,std::vector<std::string> >& structs) {
		std::map<std::string,std::vector<std::string> >::iterator it = structs.find(name);
		return (it != structs.end()) ? &it->second : 0;
	}

	//Fill up structure list and also handle structure->structure access
	static void parse_structures(std::map<std::string,std::vector<std::string> >& structs,const char* code,const uint32_t len,
					const std::vector<std::string>& base_types,
					const std::vector<std::string>& complex_types) {
		const char* pcode = code;
		std::string tok;

		for (uint32_t offs = 0;offs < len;) {
	 
			std::string field_name;
			std::vector<std::string> field_fields;

			pcode = strstr(pcode,"struct");
			if (0==pcode)
				break;
 
			offs = skip_ws(code, (ptrdiff_t)(pcode - code) + 6,len);
			if (offs>=len)
				return;

			offs = next_tok(field_name,code,offs,len);
			if (offs>=len)
				return;
			
			offs = skip_ws(code,offs,len);
			if (offs>=len)
				return;

			if (code[offs] != '{') //error
				return;
	
			offs = skip_ws(code,offs + 1,len);
			if (offs>=len)
				return;

 			std::vector<std::string>* struct_fld = 0;
			while ((offs < len)) {
				offs = skip_ws(code,offs,len);
				if (offs>=len)
					break;
				if (code[offs] == '}')
					break;
		
				offs = next_tok(tok,code,offs,len);
				if (is_builtin_type(tok,base_types,complex_types)) {
					offs = skip_ws(code,offs ,len);
					if (offs >= len)
						break;

					offs = next_tok(tok,code,offs,len);
					field_fields.push_back(tok); 
				} 
				else if ( struct_fld = get_struct_field(tok,structs))  { //Handle struct within struct
					offs = skip_ws(code,offs ,len);
					if (offs >= len)
						break;

					offs = next_tok(tok,code,offs,len);
					for (uint32_t f = 0,w = struct_fld->size();f < w;++f) {
						field_fields.push_back(tok + "." + struct_fld->at(f) ); 
						//printf("s->sadd [%s]\n",field_fields.back().c_str());
					}
				} else {
					field_fields.push_back(tok); 
					//printf("s->sadd [%s]\n",field_fields.back().c_str());
				}
				offs = skip_ws(code,offs ,len);
				if (offs >= len)
					break;

				if (code[offs] == ',')
					++offs;
				else if (code[offs] == ';')
					++offs;
				else if (code[offs] == '[') {
					++offs;
					while ((offs < len) && (code[offs] != ']'))
						++offs;
					if (offs < len)
						offs += code[offs] == ']';
				}
			}

			if ((offs < len) && (code[offs] == '}'))
				structs.insert(std::pair<std::string,std::vector<std::string> >(field_name,field_fields));

			pcode = &code[offs + 2];
			if (pcode >= &code[len])
				return;
		}
	}

	static void parse_vars(std::vector<std::string>& res,const std::string& field_name,const char* code,const uint32_t len,
					const std::vector<std::string>& base_types,
					const std::vector<std::string>& complex_types) {
		const char* pcode = code;
		std::string tok;
		const uint32_t field_size = field_name.length();
 
		std::map<std::string,std::vector<std::string> > structs;
		parse_structures(structs,code,len,base_types,complex_types);
 
		for (uint32_t offs = 0;offs < len;) {
			pcode = strstr(pcode,field_name.c_str());
			if (0==pcode)
				break;
 
			offs = (ptrdiff_t)(pcode - code );
 			pcode += field_size;
 
			uint32_t o = (ptrdiff_t)(pcode - code);
			uint32_t l = len;
			
			//for (;o < l;++o)printf("%c",code[o]); printf("\n");	 
			std::vector<std::string>* struct_fld = 0;

			o = skip_ws(code,o,l);
			if (o >= l)
				break;

			o = next_tok(tok,code,o,l);
			if (!is_builtin_type(tok,base_types,complex_types)) {			
				struct_fld = get_struct_field(tok,structs);
				if (!struct_fld) 
					continue;
			}
			//printf("add [%s]\n",tok.c_str());
			while (o < l) {
				o = skip_ws(code,o,l);
				if (o >= l)
					break;
				else if (code[o] == ';')
					break;
				o = next_tok(tok,code,o,l);

				if (struct_fld)	{
					for (uint32_t f = 0,w = struct_fld->size();f < w;++f) {
						res.push_back(tok + "." + struct_fld->at(f) ); 
						//printf("add [%s]\n",res.back().c_str());
					}
				}
				else {
					res.push_back(tok); 
					//printf("add [%s]\n",res.back().c_str());
				}
				o = skip_ws(code,o ,l);
				if (o >= l)
					break;

				if (code[o] == ',')
					++o;
				else if (code[o] == '[') {
					++o;
					while ((o < l) && (code[o] != ']'))
						++o;
					if (o < l)
						o += code[o] == ']';
				}
			}
			pcode = &code[o];
		}

 		// while(1);
	}

	public:

	jglsl_shader_c() : m_program(0) {
		import_std_builtin_types();
	}

	~jglsl_shader_c() {
		unload();
	}

	const char* get_log() const {
		return m_log_buffer.empty() ? 0 : &m_log_buffer[0];
	}
		
	void register_builtin_type(const std::string& type,const bool complex) {
		if (!complex)
			m_builtin_types.push_back(type);
		else
			m_complex_builtin_types.push_back(type);
	}

	void import_std_builtin_types() {
		m_builtin_types.clear();
		m_complex_builtin_types.clear();

		register_builtin_type("int",false);
		register_builtin_type("uint",false);
		register_builtin_type("bool",false);
		register_builtin_type("float",false);
		register_builtin_type("double",false);
		register_builtin_type("atomic_uint",false);
		register_builtin_type("vec",true);
		register_builtin_type("mat",true);
		register_builtin_type("image",true);
		register_builtin_type("sampler",true);
	}

	void unload() {
		if (m_program != 0) {
			unbind();
			glDeleteProgram(m_program);
			m_program = 0;
		}

		m_pending_attributes.clear();
		m_pending_uniforms.clear();
		m_shaders.clear();
		m_attributes.clear();
		m_uniforms.clear();
	}

	inline void bind() {
		glUseProgram(m_program);
	}

	inline void unbind() {
		glUseProgram(0);
	}

	inline uint32_t get_attribute(const std::string& attr) {
		std::map<std::string,uint32_t>::iterator it = m_attributes.find(attr);
		return (it != m_attributes.end()) ? it->second : 0;
	}

	inline uint32_t get_uniform(const std::string& uni)  {
		std::map<std::string,uint32_t>::iterator it = m_uniforms.find(uni);
		return (it != m_uniforms.end()) ? it->second : 0;
	}

	inline void add_attribute(const std::string& attr) {
		m_attributes.insert ( std::pair<std::string,uint32_t>(attr,glGetAttribLocation(m_program,attr.c_str())) );
	}

	inline void add_uniform(const std::string& uni) {
		m_uniforms.insert ( std::pair<std::string,uint32_t>(uni,glGetUniformLocation(m_program,uni.c_str())) );
	}

	bool load(const GLenum type,const char* code,const uint32_t len) {
		GLuint tmp = glCreateShader(type);
		GLint res;
		const GLint length = (const GLint)len;
		glShaderSource(tmp,1,&code,&length);
		glCompileShader(tmp);
		glGetShaderiv(tmp,GL_COMPILE_STATUS,&res);

		parse_vars(m_pending_uniforms,"uniform",code,len,m_builtin_types,m_complex_builtin_types);
		parse_vars(m_pending_attributes,"attribute",code,len,m_builtin_types,m_complex_builtin_types);
		if (res != GL_FALSE) {
			m_shaders.push_back(tmp);
			return true;
		}
 
		GLint gl_log_len;		
		glGetShaderiv(tmp, GL_INFO_LOG_LENGTH,&gl_log_len);
		GLchar * gl_log = new GLchar[gl_log_len];
		if (!gl_log)return false;
		glGetShaderInfoLog(tmp,gl_log_len,NULL,gl_log);
		
		m_log_buffer.push_back('L');
		m_log_buffer.push_back('D');
		m_log_buffer.push_back(':');
		for (GLint i = 0;i < gl_log_len;++i)
			m_log_buffer.push_back(gl_log[i]);
		m_log_buffer.push_back('\n');
		delete [] gl_log;
		return false;
	}

	bool load(const GLenum type,const std::string& code) {
		return load(type,code.c_str(),code.length());
	}

	bool finalize() {
		bool ret = true;
		GLint status;

		if (m_shaders.empty()) {
			const char* no_shdr = "finalize() : No GLSL compiled shaders found!\n";	
			const int32_t len = strlen(no_shdr);
			for (int32_t i = 0;i < len;++i)
				m_log_buffer.push_back(no_shdr[i]);
			
			return false;
		}

		if (m_program != 0) {
			char tmp[256];
			int32_t len;

			sprintf(tmp,"finalize() : Warning previous program(%u) is still active.\nShutting it down..\n",m_program);
			len = strlen(tmp);

			for (int32_t i = 0;i < len;++i)
				m_log_buffer.push_back(tmp[i]);

			unbind();
			glDeleteProgram(m_program);
		}

		m_program = glCreateProgram();
		for (uint32_t i = 0,j = m_shaders.size();i < j;++i)
			glAttachShader(m_program,m_shaders[i]);

		glLinkProgram(m_program);
		glGetProgramiv(m_program, GL_LINK_STATUS, &status);
		if (status == GL_FALSE) {
			GLint gl_log_len;
		
			glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &gl_log_len);
			GLchar* gl_log = new GLchar[gl_log_len];
			if (!gl_log) return false;
			glGetProgramInfoLog(m_program, gl_log_len, NULL, gl_log);

			m_log_buffer.push_back('L');
			m_log_buffer.push_back('N');
			m_log_buffer.push_back('K');
			m_log_buffer.push_back(':');
			for (GLint i = 0;i < gl_log_len;++i)
				m_log_buffer.push_back(gl_log[i]);
			m_log_buffer.push_back('\n');
			delete [] gl_log;
			ret = false;
		}

		for (uint32_t i = 0,j = m_shaders.size();i < j;++i)
			glDeleteShader(m_shaders[i]);

		m_attributes.clear();
		m_uniforms.clear();

		for (uint32_t i = 0,j = m_pending_attributes.size();i < j;++i)
			add_attribute(m_pending_attributes[i]);

		for (uint32_t i = 0,j = m_pending_uniforms.size();i < j;++i)
			add_uniform(m_pending_uniforms[i]);

		m_pending_attributes.clear();
		m_pending_uniforms.clear();
		m_shaders.clear();

		if (ret) 
			m_log_buffer.clear();
	
		return ret;
	}

#ifndef JGLSL_NO_GLUNIFORM_MACROS
	//By name
	template <typename scalar_t>
	inline void u_f(const std::string& name,const scalar_t f) {
		if (sizeof(scalar_t) == 4)
			glUniform1f(get_uniform(name),f);
		else
			glUniform1d(get_uniform(name),f);
	}

	template <typename scalar_t>
	inline void u_2fv(const std::string& name,const scalar_t* f)  {
		if (sizeof(scalar_t) == 4)
			glUniform2fv(get_uniform(name),1,f);
		else
			glUniform2dv(get_uniform(name),1,f);
	}

	template <typename scalar_t>
	inline void u_3fv(const std::string& name,const scalar_t* f) {
		if (sizeof(scalar_t) == 4)
			glUniform3fv(get_uniform(name),1,f);
		else
			glUniform3dv(get_uniform(name),1,f);
	}

	template <typename scalar_t>
	inline void u_4fv(const std::string& name,const scalar_t* f)  {
		if (sizeof(scalar_t) == 4)
    		glUniform4fv(get_uniform(name),1,f);
		else
    		glUniform4dv(get_uniform(name),1,f);
	}

	template <typename scalar_t>
	inline void u_mat3_fv(const std::string& name,const scalar_t* m)  {
		if (sizeof(scalar_t) == 4)
    		glUniformMatrix3fv(get_uniform(name),1,GL_FALSE,m);
		else
    		glUniformMatrix3dv(get_uniform(name),1,GL_FALSE,m);
	}

	template <typename scalar_t>
	inline void u_mat4_fv(const std::string& name,const scalar_t* m) {
		if (sizeof(scalar_t) == 4)
    		glUniformMatrix4fv(get_uniform(name),1,GL_FALSE,m);
		else
    		glUniformMatrix4dv(get_uniform(name),1,GL_FALSE,m);
	}

	inline void u_u32(const std::string& name,const uint32_t ui)  {
    	glUniform1ui(get_uniform(name),ui);
	}

	inline void u_u32v(const std::string& name,const uint32_t cnt,const uint32_t* ui) {
    	glUniform1uiv(get_uniform(name),cnt,ui);
	}

	inline void u_s32(const std::string& name,const int32_t i) {
    	glUniform1i(get_uniform(name),i);
	}

	inline void u_tex(const std::string& name,const int32_t id) {
		glUniform1i(get_uniform(name),id);
	}

	//By index (cached results by get_uniform())
	template <typename scalar_t>
	inline void u_f(const uint32_t name,const scalar_t f) const {
		if (sizeof(scalar_t) == 4)
			glUniform1f(name,f);
		else
			glUniform1d(name,f);
	}

	template <typename scalar_t>
	inline void u_2fv(const uint32_t name,const scalar_t* f) const {
		if (sizeof(scalar_t) == 4)
			glUniform2fv(name,1,f);
		else
			glUniform2dv(name,1,f);
	}

	template <typename scalar_t>
	inline void u_3fv(const uint32_t name,const scalar_t* f) const {
		if (sizeof(scalar_t) == 4)
			glUniform3fv(name,1,f);
		else
			glUniform3dv(name,1,f);
	}

	template <typename scalar_t>
	inline void u_4fv(const uint32_t name,const scalar_t* f) const {
		if (sizeof(scalar_t) == 4)
    		glUniform4fv(name,1,f);
		else
    		glUniform4dv(name,1,f);
	}

	template <typename scalar_t>
	inline void u_mat3_fv(const uint32_t name,const scalar_t* m) const {
		if (sizeof(scalar_t) == 4)
    		glUniformMatrix3fv(name,1,GL_FALSE,m);
		else
    		glUniformMatrix3dv(name,1,GL_FALSE,m);
	}

	template <typename scalar_t>
	inline void u_mat4_fv(const uint32_t name,const scalar_t* m) const {
		if (sizeof(scalar_t) == 4)
    		glUniformMatrix4fv(name,1,GL_FALSE,m);
		else
    		glUniformMatrix4dv(name,1,GL_FALSE,m);
	}

	inline void u_u32(const uint32_t name,const uint32_t ui) const {
    	glUniform1ui(name,ui);
	}

	inline void u_u32v(const uint32_t name,const uint32_t cnt,const uint32_t* ui) const {
    	glUniform1uiv(name,cnt,ui);
	}

	inline void u_s32(const uint32_t name,const int32_t i) const {
    	glUniform1i(name,i);
	}

	inline void u_tex(const uint32_t name,const int32_t id) const {
		glUniform1i(name,id);
	}
#endif
};

#endif


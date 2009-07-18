#include "Lexer.h"

void	SkipSpace(const char** str)
{
	for(;;)
	{
		while((unsigned char)((*str)[0] - 1) < ' ')
			(*str)++;
		if((*str)[0] == '/'){
			if((*str)[1] == '/')
			{
				while((*str)[0] != '\n' && (*str)[0] != '\0')
					(*str)++;
			}else if((*str)[1] == '*'){
				while(!((*str)[0] == '*' && (*str)[1] == '/') && (*str)[0] != '\0')
					(*str)++;
				(*str) += 2;
			}else{
				break;
			}
		}else{
			break;
		}
	}
}

void	Lexer::Lexify(const char* code)
{
	lexems.clear();

	while(*code)
	{
		SkipSpace(&code);
		if(!*code)
			break;

		LexemeType lType = lex_none;
		int lLength = 0;

		switch(*code)
		{
		case '\"':
			lType = lex_quotedstring;
			{
				const char *pos = code;
				pos++;
				while(!(*pos == '\"' && pos[-1] != '\\'))
					pos++;
				pos++;
				lLength = (int)(pos - code);
			}
			break;
		case '\'':
			lType = lex_semiquote;
			break;
		case '\\':
			lType = lex_escape;
			break;
		case '.':
			lType = lex_point;
			break;
		case ',':
			lType = lex_comma;
			break;
		case '+':
			lType = lex_add;
			if(code[1] == '=')
				lType = lex_addset;
			else if(code[1] == '+')
				lType = lex_inc;
			break;
		case '-':
			lType = lex_sub;
			if(code[1] == '=')
				lType = lex_subset;
			else if(code[1] == '-')
				lType = lex_dec;
			break;
		case '*':
			lType = lex_mul;
			if(code[1] == '=')
			{
				lType = lex_mulset;
			}else if(code[1] == '*'){
				lType = lex_pow;
				if(code[2] == '=')
					lType = lex_powset;
			}
			break;
		case '/':
			lType = lex_div;
			if(code[1] == '=')
				lType = lex_divset;
			break;
		case '%':
			lType = lex_mod;
			break;
		case '<':
			lType = lex_less;
			if(code[1] == '=')
				lType = lex_lequal;
			else if(code[1] == '<')
				lType = lex_shl;
			break;
		case '>':
			lType = lex_greater;
			if(code[1] == '=')
				lType = lex_gequal;
			else if(code[1] == '>')
				lType = lex_shr;
			break;
		case '=':
			lType = lex_set;
			if(code[1] == '=')
				lType = lex_equal;
			break;
		case '!':
			lType = lex_lognot;
			if(code[1] == '=')
				lType = lex_nequal;
			break;
		case '~':
			lType = lex_bitnot;
			break;
		case '&':
			lType = lex_bitand;
			break;
		case '|':
			lType = lex_bitor;
			break;
		case '^':
			lType = lex_bitxor;
			break;
		case '(':
			lType = lex_oparen;
			break;
		case ')':
			lType = lex_cparen;
			break;
		case '[':
			lType = lex_obracket;
			break;
		case ']':
			lType = lex_cbracket;
			break;
		case '{':
			lType = lex_ofigure;
			break;
		case '}':
			lType = lex_cfigure;
			break;
		case '?':
			lType = lex_questionmark;
			break;
		case ':':
			lType = lex_colon;
			break;
		case ';':
			lType = lex_semicolon;
			break;
		default:
			if(isDigit(*code))
			{
				lType = lex_number;

				const char *pos = code;
				if(pos[0] == '0' && pos[1] == 'x')
				{
					pos += 2;
					while(isDigit(*pos) || ((*pos & ~0x20) >= 'A' && (*pos & ~0x20) <= 'F'))
						pos++;
				}else{
					while(isDigit(*pos))
						pos++;
				}
				if(*pos == '.')
					pos++;
				while(isDigit(*pos))
					pos++;
				if(*pos == 'e')
					pos++;
				while(isDigit(*pos))
					pos++;
				lLength = (int)(pos - code);
			}else if(chartype_table[*code] & ct_start_symbol){
				const char *pos = code;
				while(chartype_table[*pos] & ct_symbol)
					pos++;
				lLength = (int)(pos-code);

				if(!(chartype_table[*pos] & ct_symbol))
				{
					if(lLength == 2)
					{
						if(memcmp(code, "or", 2) == 0)
							lType = lex_logor;
						else if(memcmp(code, "if", 2) == 0)
							lType = lex_if;
						else if(memcmp(code, "do", 2) == 0)
							lType = lex_do;
					}else if(lLength == 3){
						if(memcmp(code, "and", 3) == 0)
							lType = lex_logand;
						else if(memcmp(code, "xor", 3) == 0)
							lType = lex_logxor;
						else if(memcmp(code, "for", 3) == 0)
							lType = lex_for;
						else if(memcmp(code, "ref", 3) == 0)
							lType = lex_ref;
					}else if(lLength == 4){
						if(memcmp(code, "case", 4) == 0)
							lType = lex_case;
						else if(memcmp(code, "else", 4) == 0)
							lType = lex_else;
						else if(memcmp(code, "auto", 4) == 0)
							lType = lex_auto;
					}else if(lLength == 5){
						if(memcmp(code, "while", 5) == 0)
							lType = lex_while;
						else if(memcmp(code, "break", 5) == 0)
							lType = lex_break;
						else if(memcmp(code, "const", 5) == 0)
							lType = lex_const;
						else if(memcmp(code, "class", 5) == 0)
							lType = lex_class;
						else if(memcmp(code, "align", 5) == 0)
							lType = lex_align;
					}else if(lLength == 6){
						if(memcmp(code, "switch", 6) == 0)
							lType = lex_switch;
						else if(memcmp(code, "return", 6) == 0)
							lType = lex_return;
						else if(memcmp(code, "typeof", 6) == 0)
							lType = lex_typeof;
						else if(memcmp(code, "sizeof", 6) == 0)
							lType = lex_sizeof;
					}else if(lLength == 7 && memcmp(code, "noalign", 7) == 0){
						lType = lex_noalign;
					}else if(lLength == 8 && memcmp(code, "continue", 8) == 0){
						lType = lex_continue;
					}
				}
	
				if(lType == lex_none)
					lType = lex_string;
				if(!(chartype_table[*code] & ct_start_symbol))
				{
					lType = lex_none;
					lLength = 1;
					break;
				}
			}else{
				lLength = 1;	// unknown lexeme, let the parser handle
			}
		}
		if(!lLength)
			lLength = lexemLength[lType];

		Lexeme lex;
		lex.type = lType;
		lex.length = lLength;
		lex.pos = code;
		lexems.push_back(lex);

		code += lLength;
	}
	Lexeme lex;
	lex.type = lex_none;
	lex.length = 1;
	lex.pos = code;
	lexems.push_back(lex);
}

Lexeme*	Lexer::GetStreamStart()
{
	return &lexems[0];
}
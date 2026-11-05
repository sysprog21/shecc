/*
 * Token kinds and spellings share one definition list. Entry order preserves
 * token numbers. Intentionally included repeatedly with LEX_TOKEN, LEX_KEYWORD
 * and LEX_DIRECTIVE optionally defined by the caller; the rest expand to
 * nothing.
 */
/* clang-format off */
#ifndef LEX_TOKEN
#define LEX_TOKEN(kind)
#endif
#ifndef LEX_KEYWORD
#define LEX_KEYWORD(kind, spelling) LEX_TOKEN(kind)
#endif
#ifndef LEX_DIRECTIVE
#define LEX_DIRECTIVE(kind, spelling) LEX_TOKEN(kind)
#endif
LEX_TOKEN(T_start) /* FIXME: Unused, intended for lexer state machine init */
LEX_TOKEN(T_translation_unit) /* internal boundary between source input files */
LEX_TOKEN(T_eof)              /* end-of-file (EOF) */
LEX_TOKEN(T_numeric)
LEX_TOKEN(T_floating) /* C99 floating literal; lowering is staged separately */
LEX_TOKEN(T_identifier)
LEX_TOKEN(T_comma)   /* , */
LEX_TOKEN(T_string)  /* null-terminated string */
LEX_TOKEN(T_wstring) /* L"..." wide string literal */
LEX_TOKEN(T_char)
LEX_TOKEN(T_wchar)         /* L'...' wide character constant */
LEX_TOKEN(T_open_bracket)  /* ( */
LEX_TOKEN(T_close_bracket) /* ) */
LEX_TOKEN(T_open_curly)    /* { */
LEX_TOKEN(T_close_curly)   /* } */
LEX_TOKEN(T_open_square)   /* [ */
LEX_TOKEN(T_close_square)  /* ] */
LEX_TOKEN(T_asterisk)      /* '*' */
LEX_TOKEN(T_divide)        /* / */
LEX_TOKEN(T_mod)           /* % */
LEX_TOKEN(T_bit_or)        /* | */
LEX_TOKEN(T_bit_xor)       /* ^ */
LEX_TOKEN(T_bit_not)       /* ~ */
LEX_TOKEN(T_log_and)       /* && */
LEX_TOKEN(T_log_or)        /* || */
LEX_TOKEN(T_log_not)       /* ! */
LEX_TOKEN(T_lt)            /* < */
LEX_TOKEN(T_gt)            /* > */
LEX_TOKEN(T_le)            /* <= */
LEX_TOKEN(T_ge)            /* >= */
LEX_TOKEN(T_lshift)        /* << */
LEX_TOKEN(T_rshift)        /* >> */
LEX_TOKEN(T_dot)           /* . */
LEX_TOKEN(T_arrow)         /* -> */
LEX_TOKEN(T_plus)          /* + */
LEX_TOKEN(T_minus)         /* - */
LEX_TOKEN(T_minuseq)       /* -= */
LEX_TOKEN(T_pluseq)        /* += */
LEX_TOKEN(T_asteriskeq)    /* *= */
LEX_TOKEN(T_divideeq)      /* /= */
LEX_TOKEN(T_modeq)         /* %= */
LEX_TOKEN(T_lshifteq)      /* <<= */
LEX_TOKEN(T_rshifteq)      /* >>= */
LEX_TOKEN(T_xoreq)         /* ^= */
LEX_TOKEN(T_oreq)          /* |= */
LEX_TOKEN(T_andeq)         /* &= */
LEX_TOKEN(T_eq)            /* == */
LEX_TOKEN(T_noteq)         /* != */
LEX_TOKEN(T_assign)        /* = */
LEX_TOKEN(T_increment)     /* ++ */
LEX_TOKEN(T_decrement)     /* -- */
LEX_TOKEN(T_question)      /* ? */
LEX_TOKEN(T_colon)         /* : */
LEX_TOKEN(T_semicolon)     /* ; */
LEX_TOKEN(T_ampersand)     /* & */
LEX_KEYWORD(T_return, "return")
LEX_KEYWORD(T_if, "if")
LEX_KEYWORD(T_else, "else")
LEX_KEYWORD(T_while, "while")
LEX_KEYWORD(T_for, "for")
LEX_KEYWORD(T_do, "do")
LEX_KEYWORD(T_typedef, "typedef")
LEX_KEYWORD(T_enum, "enum")
LEX_KEYWORD(T_struct, "struct")
LEX_KEYWORD(T_union, "union")
LEX_KEYWORD(T_sizeof, "sizeof")
LEX_TOKEN(T_elipsis) /* ... */
LEX_KEYWORD(T_switch, "switch")
LEX_KEYWORD(T_case, "case")
LEX_KEYWORD(T_break, "break")
LEX_KEYWORD(T_default, "default")
LEX_KEYWORD(T_continue, "continue")
LEX_KEYWORD(T_goto, "goto")
LEX_KEYWORD(T_const, "const") /* const qualifier */
LEX_KEYWORD(T_volatile, "volatile")
LEX_KEYWORD(T_static, "static")
LEX_KEYWORD(T_extern, "extern")
LEX_KEYWORD(T_register, "register")
LEX_KEYWORD(T_auto, "auto")
LEX_KEYWORD(T_restrict, "restrict")
LEX_KEYWORD(T_inline, "inline")
LEX_KEYWORD(T_signed, "signed")
LEX_KEYWORD(T_unsigned, "unsigned")
LEX_KEYWORD(T_long, "long")
LEX_KEYWORD(T_float, "float")
LEX_KEYWORD(T_double, "double")
LEX_KEYWORD(T_complex, "_Complex")
LEX_KEYWORD(T_imaginary, "_Imaginary")
/* C pre-processor directives */
LEX_DIRECTIVE(T_cppd_include, "#include")
LEX_DIRECTIVE(T_cppd_define, "#define")
LEX_DIRECTIVE(T_cppd_undef, "#undef")
LEX_DIRECTIVE(T_cppd_error, "#error")
LEX_DIRECTIVE(T_cppd_warning, "#warning")
LEX_DIRECTIVE(T_cppd_if, "#if")
LEX_DIRECTIVE(T_cppd_elif, "#elif")
LEX_DIRECTIVE(T_cppd_else, "#else")
LEX_DIRECTIVE(T_cppd_endif, "#endif")
LEX_DIRECTIVE(T_cppd_ifdef, "#ifdef")
LEX_DIRECTIVE(T_cppd_ifndef, "#ifndef")
LEX_DIRECTIVE(T_cppd_pragma, "#pragma")
LEX_DIRECTIVE(T_cppd_line, "#line")
LEX_TOKEN(T_cppd_unknown) /* a directive name shecc does not support */

/* C pre-processor specific, these kinds will be removed after pre-processing is
 * done.
 */
LEX_TOKEN(T_newline)
LEX_TOKEN(T_backslash)
LEX_TOKEN(T_whitespace)
LEX_TOKEN(T_tab)

/* '#' and '##' inside a macro replacement list. Resolved while the macro is
 * expanded, so neither ever reaches the parser.
 */
LEX_TOKEN(T_hash)
LEX_TOKEN(T_hashhash)
/* clang-format on */
#undef LEX_DIRECTIVE
#undef LEX_KEYWORD
#undef LEX_TOKEN

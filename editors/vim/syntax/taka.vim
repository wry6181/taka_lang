" Vim syntax for the taka language (*.tk). Install: copy to
" ~/.vim/syntax/taka.vim and add `au BufNewFile,BufRead *.tk set ft=taka`
" to ~/.vimrc (or ftdetect/taka.vim with the same autocmd).
if exists("b:current_syntax")
  finish
endif

syn case match

" comments (block comments nest in taka; vim approximates with one level)
syn match takaLineComment "//.*$"
syn region takaBlockComment start="/\*" end="\*/" contains=takaBlockComment

" literals
syn region takaString start=/"/ skip=/\\./ end=/"/ contains=takaEscape
syn region takaChar start=/'/ skip=/\\./ end=/'/ contains=takaEscape
syn match takaEscape /\\./ contained
syn match takaFloat /\<\d[\d_]*\.\d[\d_]*\([eE][+-]\=\d[\d_]*\)\=\(f32\|f64\)\=\>/
syn match takaHex /\<0[xX][0-9a-fA-F][0-9a-fA-F_]*\(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\)\=\>/
syn match takaBin /\<0[bB][01][01_]*\(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\)\=\>/
syn match takaInt /\<\d[\d_]*\(i8\|i16\|i32\|i64\|u8\|u16\|u32\|u64\)\=\>/

" declarations / structure
syn keyword takaDecl fn struct enum effect handler trait impl dyn type extern import mod
" modifiers / visibility
syn keyword takaMod pub private const mut auto
" control flow
syn keyword takaCond where for in if else while match break continue return defer
" effect operations
syn keyword takaEffectOp perform handle with resume transact spawn
" word operators
syn keyword takaOpWord as sizeof offsetof
" primitive types
syn keyword takaType i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 char bool sview never
syn keyword takaSelf Self
" builtin effects (contextual: special inside < > rows)
syn keyword takaEffect io alloc reads writes decides suspends transacts pure ownership
" constants
syn keyword takaConst true false null undef

hi def link takaLineComment Comment
hi def link takaBlockComment Comment
hi def link takaString String
hi def link takaChar Character
hi def link takaEscape SpecialChar
hi def link takaFloat Float
hi def link takaHex Number
hi def link takaBin Number
hi def link takaInt Number
hi def link takaDecl Keyword
hi def link takaCond Conditional
hi def link takaMod StorageClass
hi def link takaEffectOp Keyword
hi def link takaOpWord Operator
hi def link takaType Type
hi def link takaSelf Type
hi def link takaEffect StorageClass
hi def link takaConst Constant

let b:current_syntax = "taka"

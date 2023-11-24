grammar slang;

file
    :   Import* namespaceDef EOF
    ;

namespaceDef
    :   'namespace' namespaceIdentifier '{' (interfaceDef | classDef)* '}'
    ;

functionHeader
    :   accessModifier ? Type Identifier  '(' functionParams? ')'
    ;

functionDef
    :   functionHeader ';'
    ;

functionImpl
    :   functionHeader codeBlock
    ;

interfaceDef
    :   'interface' Identifier '{' functionDef* '}'
    ;

classDef
    :   'class' Identifier '{' (functionImpl | classDef | interfaceDef)* '}'
    ;

accessModifier
    : 'public' | 'private' | 'protected'
    ;

field
    : accessModifier ? Type Identifier ';'
    ;

functionParams
    : functionParam (',' functionParam)*
    ;

functionParam
    :   Type Identifier
    ;

namespaceIdentifier
    :   Identifier ('::' Identifier)*
    ;

codeBlock
    : '{' statement* '}'
    ;

statement : localVariableDeclarationStatement
          | expressionStatement
          | returnStatement
          | methodCallStatement
          ;

// 定义局部变量声明语句
localVariableDeclarationStatement
    : Type Identifier ('=' expression)? ';'
    ;

// 定义表达式语句
expressionStatement
    : expression ';'
    ;

// 定义返回语句
returnStatement
    : 'return' expression? ';'
    ;

// 定义方法调用语句
methodCallStatement
    : (receiver=expression '.')? methodName=Identifier '(' arguments? ')'
    ;

// 参数列表规则
arguments
    : expression (',' expression)*
    ;

// 定义表达式
expression
    : additionExpression | Identifier | literal
    ;

// 定义字面量，例如整数或字符串字面量
literal
    : IntegerLiteral | StringLiteral
    ;

additionExpression
    : multiplicationExpression // 乘除优先于加减
    ( '+' multiplicationExpression // 加法
    | '-' multiplicationExpression // 减法
    )* ;

multiplicationExpression
    : primaryExpression // 基础表达式，例如数字、括号表达式
    ( '*' primaryExpression // 乘法
    | '/' primaryExpression // 除法
    )* ;

primaryExpression
    : '(' expression ')' // 括号表达式，用于改变优先级
    | IntegerLiteral ; // 整数

Import
    :    'import' Whitespace (ImportA | ImportB)
    ;

ImportA:
          '<' SCharSequence? '>'
        ;

ImportB:
         '"' SCharSequence? '"'
        ;

Type
    :   Void | Bool | S32 | S64 | U32 | U64 | String | Object
    ;

Void
    :   'void'
    ;

Bool
    :   'bool'
    ;

Object
    :   'object'
    ;

S32
    :   's32'
    ;

S64
    :   's64'
    ;

U32
    :   'u32'
    ;

U64
    :   'u64'
    ;

String
    :   'string'
    ;

Identifier
    :   Nondigit
        (   Nondigit
        |   Digit
        )*
    ;

Space : [ \t\r\n]+ -> skip;

fragment
SCharSequence
    :   SChar+
    ;
fragment
SChar
    :   ~["\\\r\n];

fragment
Nondigit
    :   [a-zA-Z_]
    ;

fragment
Digit
    :   [0-9]
    ;

Whitespace
    :   [ \t]+
        -> skip
    ;

Newline
    :   (   '\r' '\n'?
        |   '\n'
        )
        -> skip
    ;

BlockComment
    :   '/*' .*? '*/'
        -> skip
    ;

LineComment
    :   '//' ~[\r\n]*
        -> skip
    ;

IntegerLiteral
    : Digit+
    ;

StringLiteral
    : '"' ( ~["\\] | '\\' . )* '"'
    ;
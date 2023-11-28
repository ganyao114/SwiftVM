grammar slang;

file
    :   Import* namespaceDef EOF
    ;

namespaceDef
    :   'namespace' namespaceIdentifier '{' (interfaceDef | classDef)* '}'
    ;

functionHeader
    :   accFlag=accessModifier ? returnType=Type functionName=Identifier  '(' args=functionParams? ')'
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
    :   'class' className=Identifier '{' (functionImpl | classDef | interfaceDef | fieldDef)* '}'
    ;

accessModifier
    : 'public' | 'private' | 'protected'
    ;

fieldDef
    : accFlag=accessModifier ? fieldType=Type fieldName=Identifier ('=' expression)? ';'
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

statement : expression
          | localVariableDeclarationStatement
          | expressionStatement
          | returnStatement
          | assignmentStatement
          ;

// 定义局部变量声明语句
localVariableDeclarationStatement
    : Type localName=Identifier ('=' expression)? ';'
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
methodCallExpression
    : methodName=Identifier '(' arguments? ')'
    ;

assignmentStatement
    : receiver=Identifier '=' expression ';'
    ;

// 参数列表规则
arguments
    : argFirst=expression (',' expression)*
    ;

// 定义表达式
expression
    : literal | methodCallExpression | additionExpression | conditionExpression
    ;

// 定义字面量，例如整数或字符串字面量
literal
    : IntegerLiteral
    | StringLiteral
    | 'true'
    | 'false'
    | 'null'
    ;

conditionExpression
    : additionExpression // 乘除优先于加减
    ( '>' additionExpression
    | '<' additionExpression
    | '==' additionExpression
    | '>=' additionExpression
    | '<=' additionExpression
    )* ;

additionExpression
    : multiplicationExpression // 乘除优先于加减
    ( '+' multiplicationExpression // 加法
    | '-' multiplicationExpression // 减法
    )* ;

multiplicationExpression
    : primaryExpression // 基础表达式，例如数字、括号表达式
    ( '*' primaryExpression // 乘法
    | '/' primaryExpression // 除法
    | '<<' primaryExpression // 左移
    | '>>' primaryExpression // 右移
    )* ;

primaryExpression
    : '(' expression ')' // 括号表达式，用于改变优先级
    | literal
    | Identifier
    ; // 整数

Import
    :    'import' Whitespace '<' SCharSequence? '>'
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
    :  '"' (ESC | ~["\\])* '"'
    ;

fragment ESC :   '\\' (["\\/bfnrt] | UNICODE) ;
fragment UNICODE : 'u' HEX HEX HEX HEX ;
fragment HEX : [0-9a-fA-F] ;
enum AST_EXPRESSION {
	AST_EXPRESSION_CONSTANT,
	AST_EXPRESSION_VARARG,
	AST_EXPRESSION_FUNCTION,
	AST_EXPRESSION_VARIABLE,
	AST_EXPRESSION_FUNCTION_CALL,
	AST_EXPRESSION_TABLE,
	AST_EXPRESSION_BINARY_OPERATION,
	AST_EXPRESSION_UNARY_OPERATION
};

struct Expression {
	Ast* owner = nullptr;
	AST_EXPRESSION type;

	Expression(Ast* owner_, const AST_EXPRESSION& initialType)
		: owner(owner_), type(initialType) {
		initialize_type();
	}

	~Expression() = default;

	void set_type(const AST_EXPRESSION& newType) {
		if (type == newType) return;
		type = newType;
		initialize_type();
	}

	void initialize_type() {
		switch (type) {
		case AST_EXPRESSION_CONSTANT:
			constant = owner->new_constant();
			break;
		case AST_EXPRESSION_VARARG:
			returnCount = 0;
			break;
		case AST_EXPRESSION_FUNCTION:
			function = nullptr;
			break;
		case AST_EXPRESSION_VARIABLE:
			variable = owner->new_variable();
			break;
		case AST_EXPRESSION_FUNCTION_CALL:
			functionCall = owner->new_function_call();
			break;
		case AST_EXPRESSION_TABLE:
			table = owner->new_table();
			break;
		case AST_EXPRESSION_BINARY_OPERATION:
			binaryOperation = owner->new_binary_operation();
			break;
		case AST_EXPRESSION_UNARY_OPERATION:
			unaryOperation = owner->new_unary_operation();
			break;
		}
	}

	union {
		Constant* constant = nullptr;
		Function* function;
		Variable* variable;
		FunctionCall* functionCall;
		Table* table;
		BinaryOperation* binaryOperation;
		UnaryOperation* unaryOperation;
		uint8_t returnCount;
	};
};

enum AST_CONSTANT {
	AST_CONSTANT_NIL,
	AST_CONSTANT_FALSE,
	AST_CONSTANT_TRUE,
	AST_CONSTANT_NUMBER,
	AST_CONSTANT_CDATA_SIGNED,
	AST_CONSTANT_CDATA_UNSIGNED,
	AST_CONSTANT_CDATA_IMAGINARY,
	AST_CONSTANT_STRING
};

struct Constant {
	AST_CONSTANT type = AST_CONSTANT_NIL;

	union {
		double number;
		int64_t signed_integer;
		uint64_t unsigned_integer = 0;
	};

	std::string string;
	bool isName = false;
};

enum AST_VARIABLE {
	AST_VARIABLE_SLOT,
	AST_VARIABLE_UPVALUE,
	AST_VARIABLE_GLOBAL,
	AST_VARIABLE_TABLE_INDEX
};

struct Variable {
	AST_VARIABLE type = AST_VARIABLE_SLOT;
	uint8_t slot = 0;
	SlotScope** slotScope = nullptr;
	std::string name;
	Expression* table = nullptr;
	Expression* tableIndex = nullptr;
	bool isMultres = false;
	uint32_t multresIndex = 0;
};

struct FunctionCall {
	Expression* function = nullptr;
	std::vector<Expression*> arguments;
	Expression* multresArgument = nullptr;
	bool isMethod = false;
	uint8_t returnCount = 0;
};

struct Table {
	struct Field {
		Expression* key = nullptr;
		Expression* value = nullptr;
	};

	struct {
		std::vector<Expression*> list;
		std::vector<Field> fields;
	} constants;

	std::vector<Field> fields;
	uint32_t multresIndex = 0;
	Expression* multresField = nullptr;
};

enum AST_BINARY_OPERATION {
	AST_BINARY_ADDITION,
	AST_BINARY_SUBTRACTION,
	AST_BINARY_MULTIPLICATION,
	AST_BINARY_DIVISION,
	AST_BINARY_EXPONENTATION,
	AST_BINARY_MODULO,
	AST_BINARY_CONCATENATION,
	AST_BINARY_LESS_THAN,
	AST_BINARY_LESS_EQUAL,
	AST_BINARY_GREATER_THEN,
	AST_BINARY_GREATER_EQUAL,
	AST_BINARY_EQUAL,
	AST_BINARY_NOT_EQUAL,
	AST_BINARY_AND,
	AST_BINARY_OR
};

struct BinaryOperation {
	AST_BINARY_OPERATION type = AST_BINARY_ADDITION;
	Expression* leftOperand = nullptr;
	Expression* rightOperand = nullptr;
};

enum AST_UNARY_OPERATION {
	AST_UNARY_MINUS,
	AST_UNARY_NOT,
	AST_UNARY_LENGTH
};

struct UnaryOperation {
	AST_UNARY_OPERATION type = AST_UNARY_MINUS;
	Expression* operand = nullptr;
};

enum AST_STATEMENT {
	AST_STATEMENT_EMPTY,
	AST_STATEMENT_INSTRUCTION,
	AST_STATEMENT_RETURN,
	AST_STATEMENT_CONDITION,
	AST_STATEMENT_GOTO,
	AST_STATEMENT_NUMERIC_FOR,
	AST_STATEMENT_GENERIC_FOR,
	AST_STATEMENT_LOOP,
	AST_STATEMENT_BREAK,
	AST_STATEMENT_DECLARATION,
	AST_STATEMENT_ASSIGNMENT,
	AST_STATEMENT_FUNCTION_CALL,
	AST_STATEMENT_IF,
	AST_STATEMENT_ELSE,
	AST_STATEMENT_WHILE,
	AST_STATEMENT_REPEAT,
	AST_STATEMENT_DO,
	AST_STATEMENT_LABEL
};

struct Statement {
	Statement(const AST_STATEMENT& type) : type(type) {}

	AST_STATEMENT type;
	bool removed = false;
	
	struct {
		Bytecode::BC_OP type = Bytecode::BC_OP_INVALID;
		uint8_t a = 0;
		uint8_t b = 0;
		uint8_t c = 0;
		uint16_t d = 0;
		uint32_t id = INVALID_ID;
		uint32_t target = INVALID_ID;
		uint32_t label = INVALID_ID;
	} instruction;

	Function* function = nullptr;
	std::vector<Statement*> block;
	Local* locals = nullptr;

	struct {
		bool allowSlotSwap = false;
		bool swapped = false;
	} condition;

	struct {
		void register_slots(Expression*& expression) {
			if (openSlots.empty()) {
				openSlots.reserve(4);
			}
			openSlots.emplace_back(&expression);
		}
		template <typename... Expressions>
		void register_slots(Expression*& expression, Expressions*&... expressions) {
			if (openSlots.empty()) {
				openSlots.reserve(4);
			}
			openSlots.emplace_back(&expression);
			return register_slots(expressions...);
		}

		bool isPotentialMethod = false;
		bool isTableConstructor = false;
		bool forwardDeclaration = false;
		CONSTANT_TYPE allowedConstantType = NUMBER_CONSTANT;
		std::vector<Variable> variables;
		std::vector<Expression*> expressions;
		std::vector<Expression**> openSlots;
		Expression* multresReturn = nullptr;
	} assignment;
};

#pragma once
#include <deque>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

class Ast {
private:
	static constexpr uint32_t INVALID_ID = -1;

	enum CONSTANT_TYPE {
		INVALID_CONSTANT,
		NIL_CONSTANT,
		BOOL_CONSTANT,
		NUMBER_CONSTANT
	};

public:
	struct Local;
	struct SlotScope;
	struct ConditionGraph; 
	struct Expression;
	struct Constant;
	struct Variable;
	struct FunctionCall;
	struct Table;
	struct BinaryOperation;
	struct UnaryOperation;
	struct Statement;
	struct Function;
	
	#include "building_blocks.h"
	#include "function.h"
	#include "conditionGraph.h"
	
	std::deque<Constant> constantPool;
	std::deque<Variable> variablePool;
	std::deque<FunctionCall> functionCallPool;
	std::deque<Table> tablePool;
	std::deque<BinaryOperation> binaryOperationPool;
	std::deque<UnaryOperation> unaryOperationPool;

	Constant* new_constant();
	Variable* new_variable();
	FunctionCall* new_function_call();
	Table* new_table();
	BinaryOperation* new_binary_operation();
	UnaryOperation* new_unary_operation();

	Ast(const Bytecode& bytecode, const bool& ignoreDebugInfo, const bool& minimizeDiffs);
	~Ast();

	void operator()();

	Function* chunk = nullptr;

private:
	struct BlockInfo {
		uint32_t index = INVALID_ID;
		std::vector<Statement*>& block;
		BlockInfo* const previousBlock;
	};

	Function* new_function(const Bytecode::Prototype& prototype, const uint32_t& level);
	Statement* new_statement(const AST_STATEMENT& type);
	Expression* new_expression(const AST_EXPRESSION& type);
	
	void build_functions(Function& function, uint32_t& functionCounter);
	void build_instructions(Function& function);
	void assign_debug_info(Function& function);
	void group_jumps(Function& function);
	void build_loops(Function& function);
	void build_local_scopes(Function& function, std::vector<Statement*>& block);
	void build_expressions(Function& function, std::vector<Statement*>& block);
	void build_slot_scopes(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock);
	void eliminate_slots(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock);
	void eliminate_conditions(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock);
	void build_multi_assignment(Function& function, std::vector<Statement*>& block);
	void build_if_statements_from_map(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock, std::unordered_map<Statement*, uint32_t>& offsetMap);
	void build_if_statements(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock);
	void clean_up(Function& function);
	void clean_up_block(Function& function, std::vector<Statement*>& block, uint32_t& variableCounter, uint32_t& iteratorCounter, BlockInfo* const& previousBlock);
	
	static std::string sanitize_identifier(const std::string& name);
	std::string infer_variable_name(Function& function, const Expression* expression, const std::string& fallback);
	static bool slot_used_as_table(const std::vector<Statement*>& block, const uint8_t& slot);
	
	Expression* new_slot(const uint8_t& slot);
	Expression* new_literal(const uint8_t& literal);
	Expression* new_signed_literal(const uint16_t& signedLiteral);
	Expression* new_primitive(const uint8_t& primitive);
	Expression* new_number(const Function& function, const uint16_t& index);
	Expression* new_string(const Function& function, const uint16_t& index);
	Expression* new_table(const Function& function, const uint16_t& index);
	Expression* new_cdata(const Function& function, const uint16_t& index);

	static uint32_t get_block_index_from_id(const std::vector<Statement*>& block, const uint32_t& id);

	struct BlockIndexCache {
		static constexpr uint32_t INVALID = static_cast<uint32_t>(-1);
		std::unordered_map<uint32_t, uint32_t> idToIndex;
		bool dirty = true;
		bool usable = false;

		BlockIndexCache() { idToIndex.max_load_factor(0.7f); }
		void invalidate() { dirty = true; }
		
		void rebuild(const std::vector<Statement*>& block) {
			idToIndex.clear();
			idToIndex.reserve(block.size() * 2 + 1);
			usable = true;
			uint32_t lastValidId = INVALID;

			for (uint32_t i = 0; i < block.size(); ++i) {
				const uint32_t id = block[i]->instruction.id;
				if (id == INVALID) continue;
				if (lastValidId != INVALID && id < lastValidId) {
					usable = false;
					idToIndex.clear();
					break;
				}
				lastValidId = id;
				idToIndex[id] = i;
			}
			dirty = false;
		}

		bool try_find(const std::vector<Statement*>& block, const uint32_t id, uint32_t& outIndex) {
			if (dirty) rebuild(block);
			if (!usable) return false;
			const auto it = idToIndex.find(id);
			if (it == idToIndex.end()) { outIndex = INVALID; } 
			else { outIndex = it->second; }
			return true;
		}
	};
	
	static uint32_t get_extended_id_from_statement(Statement* const& statement);
	static uint32_t get_label_from_next_statement(Function& function, const BlockInfo& blockInfo, const bool& returnExtendedLabel, const bool& excludeDeclaration);
	static bool is_valid_block(Function& function, const BlockInfo& blockInfo, const uint32_t& blockBegin);
	static void check_valid_name(Constant* const& constant);
	void check_special_number(Expression* const& expression, const bool& isCdata = false);
	static CONSTANT_TYPE get_constant_type(Expression* const& expression);

	const Bytecode& bytecode;
	const bool ignoreDebugInfo;
	const bool minimizeDiffs;
	bool isFR2Enabled = false;
	
	std::deque<Statement> statements;
	std::deque<Function> functions;
	std::deque<Expression> expressions;
	uint64_t prototypeDataLeft = 0;
};
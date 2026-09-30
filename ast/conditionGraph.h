struct ConditionGraph {
	enum TYPE {
		ASSIGNMENT,
		STATEMENT
	} const type;

	enum class EdgeColor {
		True,
		False,
		Unconditional
	};

	struct Node {
		enum TYPE {
			LESS_THAN,
			LESS_EQUAL,
			GREATER_THEN,
			GREATER_EQUAL,
			NOT_LESS_THAN,
			NOT_LESS_EQUAL,
			NOT_GREATER_THEN,
			NOT_GREATER_EQUAL,
			EQUAL,
			NOT_EQUAL,
			TRUTHY_TEST,
			FALSY_TEST,
			BOOL_TRUTHY_TEST,
			BOOL_FALSY_TEST,
			UNCONDITIONAL_TRUE,
			UNCONDITIONAL_FALSE,
			AND,
			OR,
			NOT_AND,
			NOT_OR,
			END_TARGET,
			TRUE_TARGET,
			FALSE_TARGET,
		} type;
		Node(const TYPE& type) : type(type) {}
		uint32_t nodeLabel = INVALID_ID;
		Node* takenSucc = nullptr; // successor reached when the bytecode jump is taken
		Node* trueSucc = nullptr;
		Node* falseSucc = nullptr;
		struct PredEdge {
			Node* from;
			EdgeColor color;
		};
		std::vector<PredEdge> preds;
		bool inverted = false;
		bool fallsThrough = true;
		uint8_t twists = 0;
		std::vector<Expression*>* expressions = nullptr;
		Node* leftNode = nullptr;
		Node* rightNode = nullptr;
		Expression* resultExpression = nullptr;
		int topoIndex = -1;
		bool removed = false;
		bool isExit() const {
			return type == END_TARGET || type == TRUE_TARGET || type == FALSE_TARGET;
		}
		bool isAtom() const {
			return type < AND;
		}
	};

	ConditionGraph(const TYPE& type, Ast& ast, const uint32_t& endTargetLabel, const uint32_t& trueTargetLabel, const uint32_t& falseTargetLabel)
		: ast(ast), type(type) {
		if (type == ASSIGNMENT) {
			endTarget = new_node(Node::END_TARGET);
			endTarget->nodeLabel = endTargetLabel;
		}
		trueTarget = new_node(Node::TRUE_TARGET);
		trueTarget->nodeLabel = trueTargetLabel;
		falseTarget = new_node(Node::FALSE_TARGET);
		falseTarget->nodeLabel = falseTargetLabel;
	}

	~ConditionGraph() {
		for (uint32_t i = nodes.size(); i--;) {
			delete nodes[i];
		}
	}

	Node*& new_node(const Node::TYPE& type) {
		return nodes.emplace_back(new Node(type));
	}

	static Node::TYPE get_node_type(const Bytecode::BC_OP& instruction, const bool& swapped) {
		switch (instruction) {
		case Bytecode::BC_OP_ISLT: return swapped ? Node::GREATER_THEN : Node::LESS_THAN;
		case Bytecode::BC_OP_ISGE: return swapped ? Node::NOT_GREATER_THEN : Node::NOT_LESS_THAN;
		case Bytecode::BC_OP_ISLE: return swapped ? Node::GREATER_EQUAL : Node::LESS_EQUAL;
		case Bytecode::BC_OP_ISGT: return swapped ? Node::NOT_GREATER_EQUAL : Node::NOT_LESS_EQUAL;
		case Bytecode::BC_OP_ISEQV:
		case Bytecode::BC_OP_ISEQS:
		case Bytecode::BC_OP_ISEQN:
		case Bytecode::BC_OP_ISEQP: return Node::EQUAL;
		case Bytecode::BC_OP_ISNEV:
		case Bytecode::BC_OP_ISNES:
		case Bytecode::BC_OP_ISNEN:
		case Bytecode::BC_OP_ISNEP: return Node::NOT_EQUAL;
		case Bytecode::BC_OP_ISTC:
		case Bytecode::BC_OP_IST: return Node::TRUTHY_TEST;
		case Bytecode::BC_OP_ISFC:
		case Bytecode::BC_OP_ISF: return Node::FALSY_TEST;
		case Bytecode::BC_OP_JMP: return Node::UNCONDITIONAL_TRUE;
		default:
			assert(false, "Unsupported condition instruction", "", DEBUG_INFO);
			return Node::UNCONDITIONAL_TRUE;
		}
	}

	static uint8_t get_type_preference(const Node::TYPE& nodeType, const bool& inverted) {
		switch (nodeType) {
		case Node::LESS_THAN:
		case Node::LESS_EQUAL:
		case Node::GREATER_THEN:
		case Node::GREATER_EQUAL:
			return inverted ? 1 : 3;

		case Node::NOT_LESS_THAN:
		case Node::NOT_LESS_EQUAL:
		case Node::NOT_GREATER_THEN:
		case Node::NOT_GREATER_EQUAL:
			return inverted ? 3 : 1;

		case Node::EQUAL:
		case Node::NOT_EQUAL:
		case Node::UNCONDITIONAL_TRUE:
		case Node::UNCONDITIONAL_FALSE:
			return 3;

		case Node::TRUTHY_TEST:
			return inverted ? 2 : 3;

		case Node::FALSY_TEST:
			return inverted ? 3 : 2;

		case Node::AND:
		case Node::OR:
			return inverted ? 0 : 3;

		case Node::NOT_AND:
		case Node::NOT_OR:
			return inverted ? 3 : 0;

		default:
			return 3;
		}
	}

	void add_node(const Node::TYPE& type, const uint32_t& nodeLabel, const uint32_t& targetLabel,
		std::vector<Expression*>* const& expressions, const bool& hasAlternateTarget = false, const bool& fallsThrough = true) {
		Node* node = new_node(type);
		if (!entry) entry = node;
		node->nodeLabel = nodeLabel;
		node->expressions = expressions;
		node->fallsThrough =
			fallsThrough
			&& type != Node::UNCONDITIONAL_TRUE
			&& type != Node::UNCONDITIONAL_FALSE;
		conditionNodes.emplace_back(node);
		targetLabels[node] = targetLabel;
		alternateTargets[node] = hasAlternateTarget;
	}

	bool link_nodes() {
		const uint32_t addedCount = conditionNodes.size();
		switch (type) {
		case ASSIGNMENT:
			endAssignment = conditionNodes.back();
			conditionNodes.emplace_back(falseTarget);
			conditionNodes.emplace_back(trueTarget);
			conditionNodes.emplace_back(endTarget);
			break;
		case STATEMENT:
			conditionNodes.emplace_back(trueTarget);
			conditionNodes.emplace_back(falseTarget);
			break;
		}
		std::unordered_map<uint32_t, Node*> labelToNode;
		for (auto node : conditionNodes) {
			if (node->nodeLabel != INVALID_ID) labelToNode[node->nodeLabel] = node;
		}
		// resolve taken (jump) successors without writing edges yet
		for (uint32_t i = 0; i < addedCount; i++) {
			Node* node = conditionNodes[i];
			auto it = targetLabels.find(node);
			if (it == targetLabels.end() || it->second == INVALID_ID) continue;
			auto labelIt = labelToNode.find(it->second);
			if (labelIt == labelToNode.end()) return false;
			node->takenSucc = labelIt->second;
		}
		// orient polarity relative to exits (port of ConditionBuilder::fix_return_nodes)
		for (uint32_t i = 0; i < addedCount; i++) {
			Node* node = conditionNodes[i];
			if (!node->takenSucc) continue;
			if (node->takenSucc == endTarget) {
				node->takenSucc = node->type == Node::TRUTHY_TEST ? trueTarget : falseTarget;
				node->inverted = node->type == Node::FALSY_TEST;
			} else if (node->takenSucc == falseTarget) {
				node->inverted = true;
			} else if (node->takenSucc == trueTarget) {
				node->inverted = false;
			}
		}
		
		// port of ConditionBuilder::fix_return_nodes preference for intermediate targets.
		for (uint32_t i = 0; i < addedCount; i++) {
			Node* node = conditionNodes[i];

			if (!node->takenSucc || node->takenSucc->isExit()) continue;

			if (get_type_preference(node->type, node->inverted) != 3) {
				node->inverted = true;
			}
		}
		
		// port of ConditionBuilder::build_boolean_logic alternate-target rules
		for (uint32_t i = 0; i + 1 < addedCount; i++) {
			Node* node = conditionNodes[i];
			if (alternateTargets[node]
				&& ((i && conditionNodes[i - 1]->takenSucc == endAssignment)
					|| conditionNodes[i + 1] == endAssignment))
				node->takenSucc = endAssignment;
			if (node->takenSucc == endAssignment && i && alternateTargets[conditionNodes[i - 1]])
				conditionNodes[i - 1]->takenSucc = endAssignment;
		}
	
		// write taken edges with the uniform colour rule
		for (uint32_t i = 0; i < addedCount; i++) {
			Node* node = conditionNodes[i];
			if (node->takenSucc) link_edge(node, node->takenSucc, get_edge_color(node));
		}
		if (type == ASSIGNMENT) {
			for (uint32_t i = 0; i < addedCount; i++) {
				Node* node = conditionNodes[i];
				const bool hitsBoolTarget =
					(node->trueSucc && (node->trueSucc->type == Node::TRUE_TARGET || node->trueSucc->type == Node::FALSE_TARGET))
					|| (node->falseSucc && (node->falseSucc->type == Node::TRUE_TARGET || node->falseSucc->type == Node::FALSE_TARGET));
				if (!hitsBoolTarget) continue;
				if (node->type == Node::TRUTHY_TEST) node->type = Node::BOOL_TRUTHY_TEST;
				else if (node->type == Node::FALSY_TEST) node->type = Node::BOOL_FALSY_TEST;
			}
		}
		// fall-through (not-taken) edges in program order
		for (uint32_t i = 0; i < addedCount; i++) {
			Node* const from = conditionNodes[i];
			if (!from->fallsThrough) continue;
			
			Node* fall = (i + 1 < addedCount) ? conditionNodes[i + 1] : nullptr;
			if (!fall) {
				if (from->takenSucc == trueTarget) fall = falseTarget;
				else if (from->takenSucc == falseTarget) fall = trueTarget;
				else if (type == ASSIGNMENT && from->takenSucc == endTarget) fall = endTarget;
				else fall = (type == STATEMENT ? trueTarget : endTarget); // fallback
			}
			
			if (!fall || fall == from->takenSucc) continue;
			link_edge(from, fall, get_edge_color(from) == EdgeColor::True ? EdgeColor::False : EdgeColor::True);
		}
		conditionNodes.pop_back();
		if (type == ASSIGNMENT) conditionNodes.pop_back();
		return true;
	}

	// uniform rule: taken jump == expression true
	EdgeColor get_edge_color(Node* node) {
		return node->inverted ? EdgeColor::False : EdgeColor::True;
	}

	void link_edge(Node* from, Node* to, EdgeColor color) {
		to->preds.push_back({ from, color });
		switch (color) {
		case EdgeColor::True:
			from->trueSucc = to;
			break;
		case EdgeColor::False:
			from->falseSucc = to;
			break;
		case EdgeColor::Unconditional:
			from->trueSucc = to;
			break;
		}
	}

	bool topological_sort() {
		topo.clear();
		std::vector<Node*> visiting;
		std::unordered_set<Node*> visited;
		for (auto node : conditionNodes) {
			node->topoIndex = -1;
		}
		for (auto node : conditionNodes) {
			if (node->isExit() || node->removed || node->topoIndex >= 0) continue;
			if (!dfs_topo(node, visiting, visited)) return false;
		}
		std::reverse(topo.begin(), topo.end());
		for (uint32_t i = 0; i < topo.size(); i++) {
			topo[i]->topoIndex = static_cast<int>(i);
		}
		return true;
	}

	bool dfs_topo(Node* node, std::vector<Node*>& visiting, std::unordered_set<Node*>& visited) {
		if (visited.count(node)) return true;
		visiting.push_back(node);
		if (node->trueSucc && !node->trueSucc->isExit() && !node->trueSucc->removed) {
			if (is_on_path(visiting, node->trueSucc)) {
				visiting.pop_back();
				return false;
			}
			if (!dfs_topo(node->trueSucc, visiting, visited)) return false;
		}
		if (node->falseSucc && !node->falseSucc->isExit() && !node->falseSucc->removed) {
			if (is_on_path(visiting, node->falseSucc)) {
				visiting.pop_back();
				return false;
			}
			if (!dfs_topo(node->falseSucc, visiting, visited)) return false;
		}
		visiting.pop_back();
		visited.insert(node);
		topo.push_back(node);
		return true;
	}

	static bool is_on_path(const std::vector<Node*>& visiting, Node* node) {
		for (auto v : visiting) {
			if (v == node) return true;
		}
		return false;
	}

	bool make_monochromatic() {
		bool changed = true;
		while (changed) {
			changed = false;
			for (auto node : conditionNodes) {
				if (node->isExit() || node->removed) continue;
				bool hasTrue = false, hasFalse = false;
				for (const auto& pred : node->preds) {
					if (pred.from->removed) continue;
					if (pred.color == EdgeColor::True) hasTrue = true;
					if (pred.color == EdgeColor::False) hasFalse = true;
				}
				if (hasTrue && hasFalse) {
					if (!twist_to_resolve(node)) return false;
					changed = true;
				}
			}
		}
		return true;
	}

	bool twist_to_resolve(Node* node) {
		int trueCount = 0;
		int falseCount = 0;
		for (const auto& predEdge : node->preds) {
			if (predEdge.from->removed) continue;
			if (predEdge.color == EdgeColor::True) {
				trueCount++;
			} else if (predEdge.color == EdgeColor::False) {
				falseCount++;
			}
		}
		if (!trueCount || !falseCount) return true;

		EdgeColor targetColor = trueCount >= falseCount
			? EdgeColor::True
			: EdgeColor::False;

		for (const auto& predEdge : node->preds) {
			if (predEdge.from->removed) continue;
			if (predEdge.color == targetColor) continue;
			Node* pred = predEdge.from;
			if (!pred->isAtom() || pred->twists) return false;
			twist_node(pred);
		}
		return true;
	}

	void twist_node(Node* node) {
		node->twists++;
		node->inverted = !node->inverted;
		std::swap(node->trueSucc, node->falseSucc);
		recolor_incoming(node->trueSucc, node, EdgeColor::True);
		recolor_incoming(node->falseSucc, node, EdgeColor::False);
	}
	
	void reset_all_twists() {
		for (auto node : conditionNodes) {
			// If a node was twisted an odd number of times, twist it one more time
			if (node->twists % 2 != 0) {
				twist_node(node);
			}
		}
	}

	void recolor_incoming(Node* succ, Node* pred, EdgeColor newColor) {
		if (!succ) return;
		for (auto& predEdge : succ->preds) {
			if (predEdge.from == pred) predEdge.color = newColor;
		}
	}

	void deduplicate_preds(Node* node) {
		if (!node) return;
		std::unordered_set<Node*> seen;
		for (auto it = node->preds.begin(); it != node->preds.end();) {
			if (seen.count(it->from)) it = node->preds.erase(it);
			else {
				seen.insert(it->from);
				++it;
			}
		}
	}

	int count_incoming_edges(Node* node, EdgeColor color) {
		if (!node) return 0;
		int count = 0;
		for (const auto& pe : node->preds) {
			if (!pe.from->removed && pe.color == color) count++;
		}
		return count;
	}

	Node* follow_edges(Node* root, EdgeColor color, int count) {
		Node* current = root;
		for (int i = 0; i < count; i++) {
			if (!current) return nullptr;
			current = color == EdgeColor::True ? current->trueSucc : current->falseSucc;
		}
		return current;
	}

	Node* find_anchor(Node* NZ, EdgeColor color, int rootTopoIndex) {
		if (!NZ) return nullptr;
		Node* anchor = nullptr;
		int maxTopo = rootTopoIndex;
		for (const auto& pe : NZ->preds) {
			if (pe.from->removed) continue;
			if (pe.color == color && pe.from->topoIndex > maxTopo) {
				maxTopo = pe.from->topoIndex;
				anchor = pe.from;
			}
		}
		return anchor;
	}

	bool reduce_diamond() {
		for (auto c0 : topo) {
			if (c0->isExit() || c0->removed || !c0->trueSucc || !c0->falseSucc) continue;
			Node* c1 = c0->trueSucc;
			Node* c2 = c0->falseSucc;
			if (c1 == c2) continue;
			if (c1->isExit() || c2->isExit() || c1->removed || c2->removed) continue;
			if (!c1->trueSucc || !c1->falseSucc || !c2->trueSucc || !c2->falseSucc) continue;
			
			Node* trueExit = nullptr;
			Node* falseExit = nullptr;

			// Diamond
			if (c1->trueSucc == c2->trueSucc && c1->falseSucc == c2->falseSucc) {
				trueExit = c1->trueSucc;
				falseExit = c1->falseSucc;
			} 
			// XOR / Inequality Diamond
			else if (c1->trueSucc == c2->falseSucc && c1->falseSucc == c2->trueSucc) {
				trueExit = c1->trueSucc;
				falseExit = c1->falseSucc;
			} 
			else {
				continue;
			}
			
			Expression* c0_expr = build_expression(c0);
			Expression* c1_expr = build_expression(c1);
			Expression* c2_expr = build_expression(c2);
			
			if (!c0_expr || !c1_expr || !c2_expr) continue;
			
			// build_ite automatically generates the correct short-circuiting AST:
			// (c0 AND c1) OR (NOT c0 AND c2)
			Expression* combined_expr = build_ite(c0_expr, c1_expr, c2_expr);
			if (!combined_expr) continue;
			
			Node* combined = new_node(Node::OR); 
			combined->nodeLabel = c0->nodeLabel;
			combined->trueSucc = trueExit;
			combined->falseSucc = falseExit;
			combined->resultExpression = combined_expr;
			
			std::vector<Node*> removedNodes = {c0, c1, c2};
			replace_nodes(removedNodes, combined);
			
			// Clean up the predecessor lists of the exit nodes
			for (auto it = trueExit->preds.begin(); it != trueExit->preds.end(); ) {
				if (it->from == c1 || it->from == c2) it = trueExit->preds.erase(it);
				else ++it;
			}
			for (auto it = falseExit->preds.begin(); it != falseExit->preds.end(); ) {
				if (it->from == c1 || it->from == c2) it = falseExit->preds.erase(it);
				else ++it;
			}
			trueExit->preds.push_back({combined, EdgeColor::True});
			falseExit->preds.push_back({combined, EdgeColor::False});
			
			return topological_sort();
		}
		return false;
	}

	bool reduce_and_or() {
		for (uint32_t ti = 0; ti < topo.size(); ti++) {
			Node* root = topo[ti];
			if (root->isExit() || root->removed || !root->trueSucc || !root->falseSucc) continue;
			Node* NG = root->trueSucc;
			Node* NR = root->falseSucc;
			const int ng = count_incoming_edges(NG, EdgeColor::True);
			const int nr = count_incoming_edges(NR, EdgeColor::False);
			if (ng > 1 && nr <= 1) {
				Node* NZ = follow_edges(root, EdgeColor::False, ng);
				if (!NZ) continue;
				Node* NA = find_anchor(NZ, EdgeColor::False, static_cast<int>(ti));
				if (!NA || NA == root) continue;
				Expression* expr = build_chain_expression(root, NA, false);
				if (!expr) continue;
				create_combined_node(root, NA, expr, Node::OR);
				return topological_sort();
			}
			if (nr > 1 && ng <= 1) {
				Node* NZ = follow_edges(root, EdgeColor::True, nr);
				if (!NZ) continue;
				Node* NA = find_anchor(NZ, EdgeColor::True, static_cast<int>(ti));
				if (!NA || NA == root) continue;
				Expression* expr = build_chain_expression(root, NA, true);
				if (!expr) continue;
				create_combined_node(root, NA, expr, Node::AND);
				return topological_sort();
			}
			if (ng == 1 && nr == 1) {
				if (try_reduce_simple(root)) return topological_sort();
			}
		}
		return false;
	}

	bool try_reduce_simple(Node* root) {
		Node* NG = root->trueSucc;
		Node* NR = root->falseSucc;
		if (!NG->isExit() && !NG->removed && (NR == falseTarget || NR->isExit())) {
			Node* combined = new_node(Node::AND);
			combined->nodeLabel = root->nodeLabel;
			combined->leftNode = copy_node_shallow(root);
			combined->rightNode = copy_node_shallow(NG);
			combined->trueSucc = NG->trueSucc;
			combined->falseSucc = NG->falseSucc ? NG->falseSucc : falseTarget;
			
			replace_nodes({root, NG}, combined);
			return true;
		}
		if ((NG == trueTarget || NG->isExit()) && !NR->isExit() && !NR->removed) {
			Node* combined = new_node(Node::OR);
			combined->nodeLabel = root->nodeLabel;
			combined->leftNode = copy_node_shallow(root);
			combined->rightNode = copy_node_shallow(NR);
			combined->trueSucc = NR->trueSucc ? NR->trueSucc : trueTarget;
			combined->falseSucc = NR->falseSucc;
			
			replace_nodes({root, NR}, combined);
			return true;
		}
		return false;
	}

	Expression* build_chain_expression(Node* root, Node* anchor, bool isAnd) {
		std::vector<Node*> chain;
		Node* current = root;
		uint32_t guard = 0;

		while (current && current != anchor && guard++ <= conditionNodes.size()) {
			if (current->removed || current->isExit()) return nullptr;
			chain.push_back(current);
			current = isAnd
				? current->trueSucc
				: current->falseSucc;
		}

		if (current != anchor || anchor->removed || anchor->isExit()) return nullptr;
		chain.push_back(anchor);
		if (!chain.size()) return nullptr;
		if (chain.size() == 1) return build_expression(chain[0]);
		return build_expression_recursive(chain, 0, (uint32_t)chain.size() - 1, isAnd);
	}

	// operators come from the actual edges, never from a passed flag
	Expression* build_expression_recursive(const std::vector<Node*>& chain, uint32_t start, uint32_t end, bool isAnd) {
		if (start > end) return nullptr;
		if (start == end) return build_expression(chain[start]);
		Node* first = chain[start];
		Node* second = chain[start + 1];
		const Node::TYPE sequential = first->falseSucc == second ? Node::OR
			: first->trueSucc == second ? Node::AND
			: (isAnd ? Node::AND : Node::OR);
		if (end - start == 1) {
			return build_binary(sequential, build_expression(chain[start]), build_expression(chain[end]));
		}
		bool greenSkips = first->trueSucc && !first->trueSucc->isExit() && first->trueSucc->topoIndex > second->topoIndex;
		bool redSkips = first->falseSucc && !first->falseSucc->isExit() && first->falseSucc->topoIndex > second->topoIndex;
		if (greenSkips && !redSkips) {
			uint32_t splitPoint = end;
			bool found = false;
			for (uint32_t i = start + 1; i <= end; i++) {
				if (chain[i] == first->trueSucc || chain[i]->topoIndex >= first->trueSucc->topoIndex) {
					splitPoint = i;
					found = true;
					break;
				}
			}
			if (found && splitPoint > start) {
				return build_binary(Node::AND,
					build_expression_recursive(chain, start, splitPoint - 1, false),
					build_expression_recursive(chain, splitPoint, end, isAnd));
			}
		}
		if (redSkips && !greenSkips) {
			uint32_t splitPoint = end;
			bool found = false;
			for (uint32_t i = start + 1; i <= end; i++) {
				if (chain[i] == first->falseSucc || chain[i]->topoIndex >= first->falseSucc->topoIndex) {
					splitPoint = i;
					found = true;
					break;
				}
			}
			if (found && splitPoint > start) {
				return build_binary(Node::OR,
					build_expression_recursive(chain, start, splitPoint - 1, true),
					build_expression_recursive(chain, splitPoint, end, isAnd));
			}
		}
		return build_binary(sequential, build_expression(chain[start]), build_expression_recursive(chain, start + 1, end, isAnd));
	}

	Node* create_combined_node(Node* root, Node* anchor, Expression* expr, Node::TYPE opType) {
		Node* combined = new_node(opType);
		combined->nodeLabel = root->nodeLabel;
		combined->trueSucc = anchor->trueSucc;
		combined->falseSucc = anchor->falseSucc;
		combined->resultExpression = expr;
		
		std::vector<Node*> removedNodes;
		Node* current = root;
		uint32_t guard = 0;

		while (current && guard++ <= conditionNodes.size()) {
			removedNodes.push_back(current);
			if (current == anchor) break;
			current = opType == Node::AND
				? current->trueSucc
				: current->falseSucc;
		}
		
		replace_nodes(removedNodes, combined);
		return combined;
	}

	Node* copy_node_shallow(Node* node) {
		Node* copy = new_node(node->type);
		copy->inverted = node->inverted;
		copy->expressions = node->expressions;
		copy->leftNode = node->leftNode;
		copy->rightNode = node->rightNode;
		copy->resultExpression = node->resultExpression;
		copy->nodeLabel = node->nodeLabel;
		return copy;
	}

	void update_pred_references(Node* oldNode, Node* newNode) {
		for (auto& pe : newNode->preds) {
			if (pe.from->trueSucc == oldNode) pe.from->trueSucc = newNode;
			if (pe.from->falseSucc == oldNode) pe.from->falseSucc = newNode;
		}
	}

	void update_succ_references(Node* oldNode, Node* newNode) {
		if (newNode->trueSucc) {
			for (auto& pe : newNode->trueSucc->preds) {
				if (pe.from == oldNode) pe.from = newNode;
			}
		}
		if (newNode->falseSucc) {
			for (auto& pe : newNode->falseSucc->preds) {
				if (pe.from == oldNode) pe.from = newNode;
			}
		}
	}

	void replace_in_condition_nodes(Node* oldNode, Node* newNode) {
		if (entry == oldNode) entry = newNode;

		for (uint32_t i = 0; i < conditionNodes.size(); i++) {
			if (conditionNodes[i] == oldNode) {
				conditionNodes[i] = newNode;
				return;
			}
		}
	}

	void remove_from_condition_nodes(Node* node) {
		if (entry == node) entry = nullptr;

		conditionNodes.erase(
			std::remove(conditionNodes.begin(), conditionNodes.end(), node),
			conditionNodes.end());
	}

	static bool is_true_expression(const Expression* expression) {
		return expression
			&& expression->type == AST_EXPRESSION_CONSTANT
			&& expression->constant->type == AST_CONSTANT_TRUE;
	}

	static bool is_false_expression(const Expression* expression) {
		return expression
			&& expression->type == AST_EXPRESSION_CONSTANT
			&& expression->constant->type == AST_CONSTANT_FALSE;
	}

	Expression* build_ite(Expression* cond, Expression* trueExpr, Expression* falseExpr) {
		if (!cond || !trueExpr || !falseExpr) return nullptr;

		if (trueExpr == falseExpr) return trueExpr;

		if (is_true_expression(trueExpr) && is_false_expression(falseExpr)) {
			return cond;
		}

		if (is_false_expression(trueExpr) && is_true_expression(falseExpr)) {
			return build_not(cond);
		}

		if (is_true_expression(trueExpr)) {
			return build_binary(Node::OR, cond, falseExpr);
		}

		if (is_false_expression(falseExpr)) {
			return build_binary(Node::AND, cond, trueExpr);
		}

		if (is_false_expression(trueExpr)) {
			return build_binary(Node::AND, build_not(cond), falseExpr);
		}

		if (is_true_expression(falseExpr)) {
			return build_binary(Node::OR, build_not(cond), trueExpr);
		}

		return build_binary(
			Node::OR,
			build_binary(Node::AND, cond, trueExpr),
			build_binary(Node::AND, build_not(cond), falseExpr)
		);
	}

	Expression* build_generic_expression(
		Node* node,
		std::unordered_map<Node*, Expression*>& memo,
		std::unordered_set<Node*>& visiting
	) {
		if (!node) return nullptr;

		if (node == trueTarget) return ast.new_primitive(2);
		if (node == falseTarget) return ast.new_primitive(1);
		if (node == endTarget) return ast.new_primitive(1);

		if (node->resultExpression) return node->resultExpression;
		if (node->removed) return nullptr;

		auto memoIt = memo.find(node);
		if (memoIt != memo.end()) return memoIt->second;

		if (visiting.count(node)) return nullptr;
		visiting.insert(node);

		Expression* result = nullptr;

		if (node->type == Node::UNCONDITIONAL_TRUE || node->type == Node::UNCONDITIONAL_FALSE) {
			Node* next = node->trueSucc ? node->trueSucc : node->falseSucc;

			if (!next) {
				next = node->type == Node::UNCONDITIONAL_TRUE ? trueTarget : falseTarget;
			}

			result = build_generic_expression(next, memo, visiting);
		} else {
			Expression* cond = build_expression(node);

			if (cond) {
				Node* trueNode = node->trueSucc ? node->trueSucc : trueTarget;
				Node* falseNode = node->falseSucc ? node->falseSucc : falseTarget;

				Expression* trueExpr = build_generic_expression(trueNode, memo, visiting);
				Expression* falseExpr = build_generic_expression(falseNode, memo, visiting);

				result = build_ite(cond, trueExpr, falseExpr);
			}
		}
		visiting.erase(node);
		memo[node] = result;
		return result;
	}

	bool reduce_generic() {
		Node* root = nullptr;

		// Prefer a node with no active incoming edges.
		for (auto node : conditionNodes) {
			if (node->removed || node->isExit()) continue;

			bool hasActivePred = false;

			for (const auto& predEdge : node->preds) {
				if (!predEdge.from->removed) {
					hasActivePred = true;
					break;
				}
			}

			if (!hasActivePred) {
				root = node;
				break;
			}
		}

		// Fallback to tracked entry.
		if (!root || root->removed || root->isExit()) {
			root = entry;

			if (!root || root->removed || root->isExit()) {
				root = nullptr;

				for (auto node : conditionNodes) {
					if (!node->removed && !node->isExit()) {
						root = node;
						break;
					}
				}
			}
		}

		if (!root) return false;

		std::unordered_map<Node*, Expression*> memo;
		std::unordered_set<Node*> visiting;

		Expression* expression = build_generic_expression(root, memo, visiting);

		if (!expression) return false;

		Node* combined = new_node(Node::OR);

		combined->nodeLabel = root->nodeLabel;
		combined->trueSucc = trueTarget;
		combined->falseSucc = falseTarget;
		combined->resultExpression = expression;

		for (auto node : conditionNodes) {
			if (!node->isExit()) node->removed = true;
		}

		conditionNodes.clear();
		conditionNodes.emplace_back(combined);

		entry = combined;

		return true;
	}

	void replace_nodes(const std::vector<Node*>& removedNodes, Node* newNode) {
		std::unordered_set<Node*> removedSet(removedNodes.begin(), removedNodes.end());
		
		if (newNode->trueSucc && removedSet.count(newNode->trueSucc)) {
			newNode->trueSucc = trueTarget; 
		}
		if (newNode->falseSucc && removedSet.count(newNode->falseSucc)) {
			newNode->falseSucc = falseTarget;
		}

		// Update all successors in the entire graph to point to newNode
		for (Node* node : conditionNodes) {
			if (node->removed) continue;
			if (node->trueSucc && removedSet.count(node->trueSucc)) {
				node->trueSucc = newNode;
			}
			if (node->falseSucc && removedSet.count(node->falseSucc)) {
				node->falseSucc = newNode;
			}
		}
		
		// Collect predecessors from all removed nodes and transfer them to newNode
		for (Node* removed : removedNodes) {
			for (const auto& pe : removed->preds) {
				if (!pe.from->removed && !removedSet.count(pe.from)) {
					newNode->preds.push_back({pe.from, pe.color});
				}
			}
			removed->removed = true;
		}
		
		// Deduplicate predecessors
		deduplicate_preds(newNode);
		
		// Update conditionNodes list and entry point
		bool wasEntry = entry && removedSet.count(entry);
		
		for (Node* removed : removedNodes) {
			conditionNodes.erase(
				std::remove(conditionNodes.begin(), conditionNodes.end(), removed),
				conditionNodes.end());
		}
		
		bool found = false;
		for (Node* n : conditionNodes) {
			if (n == newNode) { found = true; break; }
		}
		if (!found) conditionNodes.push_back(newNode);
		
		if (wasEntry) {
			entry = newNode;
		}
	}

	bool reduce_patterns() {
		bool changed = true;

		while (changed) {
			changed = false;

			if (reduce_diamond()) {
				changed = true;
				continue;
			}

			if (reduce_and_or()) {
				changed = true;
				continue;
			}
		}

		uint32_t activeCount = 0;

		for (auto node : conditionNodes) {
			if (!node->removed && !node->isExit()) {
				activeCount++;
			}
		}

		if (activeCount == 0) return false;

		if (activeCount != 1) {
			if (!reduce_generic()) {
				print("ConditionGraph failure: reduce_generic failed on active graph");
				return false;
			}
		}

		conditionNodes.erase(
			std::remove_if(conditionNodes.begin(), conditionNodes.end(),
				[](Node* n) { return n->removed; }),
			conditionNodes.end());

		conditionNodes.erase(
			std::remove_if(conditionNodes.begin(), conditionNodes.end(),
				[](Node* n) { return n->isExit(); }),
			conditionNodes.end());

		return conditionNodes.size() == 1;
	}

	Expression* build_expression(Node* node) {
		if (!node) return nullptr;
		if (node->resultExpression) return node->resultExpression;
		switch (node->type) {
		case Node::LESS_THAN:
		case Node::LESS_EQUAL:
		case Node::GREATER_THEN:
		case Node::GREATER_EQUAL:
			return node->inverted ? build_not(build_binary(node->type, (*node->expressions)[0], (*node->expressions)[1]))
				: build_binary(node->type, (*node->expressions)[0], (*node->expressions)[1]);
		case Node::NOT_LESS_THAN:
		case Node::NOT_LESS_EQUAL:
		case Node::NOT_GREATER_THEN:
		case Node::NOT_GREATER_EQUAL:
			return node->inverted ? build_binary(node->type, (*node->expressions)[0], (*node->expressions)[1])
				: build_not(build_binary(node->type, (*node->expressions)[0], (*node->expressions)[1]));
		case Node::EQUAL:
			return build_binary(node->inverted ? Node::NOT_EQUAL : Node::EQUAL, (*node->expressions)[0], (*node->expressions)[1]);
		case Node::NOT_EQUAL:
			return build_binary(node->inverted ? Node::EQUAL : Node::NOT_EQUAL, (*node->expressions)[0], (*node->expressions)[1]);
		case Node::TRUTHY_TEST:
			return node->inverted
				? build_not((*node->expressions).back())
				: (*node->expressions).back();
		case Node::FALSY_TEST:
			return node->inverted
				? (*node->expressions).back()
				: build_not((*node->expressions).back());
		case Node::BOOL_TRUTHY_TEST:
			return node->inverted
				? build_not((*node->expressions).back())
				: build_not(build_not((*node->expressions).back()));
		case Node::BOOL_FALSY_TEST:
			return node->inverted
				? build_not(build_not((*node->expressions).back()))
				: build_not((*node->expressions).back());
		case Node::UNCONDITIONAL_TRUE:
			return ast.new_primitive(node->inverted ? 1 : 2);
		case Node::UNCONDITIONAL_FALSE:
			return ast.new_primitive(node->inverted ? 2 : 1);
		case Node::AND:
		case Node::OR:
			if (node->leftNode && node->rightNode) {
				return node->inverted ? build_not(build_binary(node->type, build_expression(node->leftNode), build_expression(node->rightNode)))
					: build_binary(node->type, build_expression(node->leftNode), build_expression(node->rightNode));
			}
			return nullptr;
		case Node::NOT_AND:
		case Node::NOT_OR:
			if (node->leftNode && node->rightNode) {
				return node->inverted ? build_binary(node->type, build_expression(node->leftNode), build_expression(node->rightNode))
					: build_not(build_binary(node->type, build_expression(node->leftNode), build_expression(node->rightNode)));
			}
			return nullptr;
		default:
			assert(false, "Unhandled condition graph node type", ast.bytecode.filePath, DEBUG_INFO);
			return nullptr;
		}
	}

	Expression* build_not(Expression* const& operand) {
		if (operand->type == AST_EXPRESSION_UNARY_OPERATION && operand->unaryOperation->type == AST_UNARY_NOT) {
			return operand->unaryOperation->operand;
		}
		
		Expression* const expression = ast.new_expression(AST_EXPRESSION_UNARY_OPERATION);
		expression->unaryOperation->type = AST_UNARY_NOT;
		expression->unaryOperation->operand = operand;
		return expression;
	}

	Expression* build_binary(const Node::TYPE& type, Expression* const& leftOperand, Expression* const& rightOperand) {
		assert(leftOperand && rightOperand, "Condition graph produced a null operand", ast.bytecode.filePath, DEBUG_INFO);
		Expression* const expression = ast.new_expression(AST_EXPRESSION_BINARY_OPERATION);
		switch (type) {
		case Node::LESS_THAN:
		case Node::NOT_LESS_THAN:
			expression->binaryOperation->type = AST_BINARY_LESS_THAN;
			break;
		case Node::LESS_EQUAL:
		case Node::NOT_LESS_EQUAL:
			expression->binaryOperation->type = AST_BINARY_LESS_EQUAL;
			break;
		case Node::GREATER_THEN:
		case Node::NOT_GREATER_THEN:
			expression->binaryOperation->type = AST_BINARY_GREATER_THEN;
			break;
		case Node::GREATER_EQUAL:
		case Node::NOT_GREATER_EQUAL:
			expression->binaryOperation->type = AST_BINARY_GREATER_EQUAL;
			break;
		case Node::EQUAL:
			expression->binaryOperation->type = AST_BINARY_EQUAL;
			break;
		case Node::NOT_EQUAL:
			expression->binaryOperation->type = AST_BINARY_NOT_EQUAL;
			break;
		case Node::AND:
		case Node::NOT_AND:
			expression->binaryOperation->type = AST_BINARY_AND;
			break;
		case Node::OR:
		case Node::NOT_OR:
			expression->binaryOperation->type = AST_BINARY_OR;
			break;
		default:
			assert(false, "Unhandled condition graph binary node type", ast.bytecode.filePath, DEBUG_INFO);
			return nullptr;
		}
		expression->binaryOperation->leftOperand = leftOperand;
		expression->binaryOperation->rightOperand = rightOperand;
		return expression;
	}

	Expression* build_condition() {
		if (!link_nodes()) {
			print("ConditionGraph failure: link_nodes");
			return nullptr;
		}
		if (!topological_sort()) {
			print("ConditionGraph failure: topological_sort");
			return nullptr;
		}
		if (!make_monochromatic()) {
			reset_all_twists();
		}
		if (!reduce_patterns()) {
			print("ConditionGraph failure: reduce_patterns ("
				+ std::to_string(conditionNodes.size())
				+ " nodes left)");
			return nullptr;
		}
		if (conditionNodes.empty()) {
			print("ConditionGraph failure: no remaining nodes after reduction");
			return nullptr;
		}
		Node* node = conditionNodes.back();
		// orient the final expression toward the then-branch / true value
		if (node->trueSucc == falseTarget) {
			if (node->resultExpression) node->resultExpression = build_not(node->resultExpression);
			else node->inverted = !node->inverted;
			std::swap(node->trueSucc, node->falseSucc);
		}
		return build_expression(node);
	}

	Ast& ast;
	std::vector<Node*> nodes;
	std::vector<Node*> conditionNodes;
	std::vector<Node*> topo;
	std::unordered_map<Node*, uint32_t> targetLabels;
	std::unordered_map<Node*, bool> alternateTargets;
	Node* endAssignment = nullptr;
	Node* endTarget = nullptr;
	Node* trueTarget = nullptr;
	Node* falseTarget = nullptr;
	Node* entry = nullptr;
};
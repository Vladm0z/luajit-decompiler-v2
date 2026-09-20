struct Ast::ConditionGraph {
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
		Node* trueSucc = nullptr;
		Node* falseSucc = nullptr;
		struct PredEdge {
			Node* from;
			EdgeColor color;
		};
		std::vector<PredEdge> preds;
		bool inverted = false;
		std::vector<Expression*>* expressions = nullptr;
		Node* leftNode = nullptr;
		Node* rightNode = nullptr;
		uint8_t twists = 0; 
		Expression* resultExpression = nullptr;
		int topoIndex = -1;
		bool removed = false;
		bool isExit() const {
			return type == END_TARGET || type == TRUE_TARGET || type == FALSE_TARGET;
		}
		bool isAtom() const {
			return type < AND;
		}
		bool fallsThrough = true;
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
		default: throw nullptr;
		}
	}

	void add_node(const Node::TYPE& type, const uint32_t& nodeLabel, const uint32_t& targetLabel,
		std::vector<Expression*>* const& expressions, const bool& hasAlternateTarget = false, const bool& fallsThrough = true) {
		Node* node = new_node(type);
		node->nodeLabel = nodeLabel;
		node->expressions = expressions;
		node->fallsThrough = fallsThrough;
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
			if (node->nodeLabel != INVALID_ID) {
				labelToNode[node->nodeLabel] = node;
			}
		}

		for (auto node : conditionNodes) {
			auto it = targetLabels.find(node);
			if (it == targetLabels.end() || it->second == INVALID_ID) continue;
			auto labelIt = labelToNode.find(it->second);
			if (labelIt == labelToNode.end()) return false;
			link_edge(node, labelIt->second, get_edge_color(node));
		}

		if (type == ASSIGNMENT) {
			for (auto node : conditionNodes) {
				if (node->isExit()) continue;
				const bool hitsBoolTarget =
					(node->trueSucc && (node->trueSucc->type == Node::TRUE_TARGET || node->trueSucc->type == Node::FALSE_TARGET))
					|| (node->falseSucc && (node->falseSucc->type == Node::TRUE_TARGET || node->falseSucc->type == Node::FALSE_TARGET));
				if (!hitsBoolTarget) continue;
				if (node->type == Node::TRUTHY_TEST) node->type = Node::BOOL_TRUTHY_TEST;
				else if (node->type == Node::FALSY_TEST) node->type = Node::BOOL_FALSY_TEST;
			}
		}

		for (uint32_t i = 0; i < addedCount; i++) {
			Node* const from = conditionNodes[i];
			if (!from->fallsThrough) continue;
			Node* const fall = i + 1 < addedCount ? conditionNodes[i + 1] : (type == STATEMENT ? trueTarget : endTarget);
			if (!fall) continue;
			const EdgeColor fallColor = get_edge_color(from) == EdgeColor::True ? EdgeColor::False : EdgeColor::True;
			if (fallColor == EdgeColor::True ? from->trueSucc == nullptr : from->falseSucc == nullptr)
				link_edge(from, fall, fallColor);
		}

		conditionNodes.pop_back();
		if (type == ASSIGNMENT) conditionNodes.pop_back();
		return true;
	}

	EdgeColor get_edge_color(Node* node) {
		switch (node->type) {
		case Node::TRUTHY_TEST:
		case Node::BOOL_TRUTHY_TEST:
		case Node::UNCONDITIONAL_TRUE:
			return EdgeColor::True;
		case Node::FALSY_TEST:
		case Node::BOOL_FALSY_TEST:
		case Node::UNCONDITIONAL_FALSE:
			return EdgeColor::False;
		default:
			return node->inverted ? EdgeColor::False : EdgeColor::True;
		}
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
		EdgeColor targetColor = EdgeColor::True;
		bool found = false;
		for (const auto& predEdge : node->preds) {
			if (predEdge.from->removed) continue;
			targetColor = predEdge.color;
			found = true;
			break;
		}
		if (!found) return true;
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

	bool reduce_ternary() {
		for (auto c0 : topo) {
			if (c0->isExit() || c0->removed || !c0->trueSucc || !c0->falseSucc) continue;
			Node* c1 = c0->trueSucc;
			Node* c2 = c0->falseSucc;
			if (c1->isExit() || c2->isExit() || c1->removed || c2->removed) continue;
			if (!c1->trueSucc || !c1->falseSucc || !c2->trueSucc || !c2->falseSucc) continue;
			if (c1->trueSucc != c2->trueSucc || c1->falseSucc != c2->falseSucc) continue;

			Node* combined = new_node(Node::OR);
			combined->nodeLabel = c0->nodeLabel;
			combined->trueSucc = c1->trueSucc;
			combined->falseSucc = c1->falseSucc;

			Node* leftAnd = new_node(Node::AND);
			leftAnd->leftNode = copy_node_shallow(c0);
			leftAnd->rightNode = copy_node_shallow(c1);

			Node* rightAnd = new_node(Node::AND);
			Node* notC0 = copy_node_shallow(c0);
			notC0->inverted = !notC0->inverted;
			rightAnd->leftNode = notC0;
			rightAnd->rightNode = copy_node_shallow(c2);

			combined->leftNode = leftAnd;
			combined->rightNode = rightAnd;

			combined->preds = c0->preds;
			for (auto& pe : combined->preds) {
				if (pe.from->trueSucc == c0) pe.from->trueSucc = combined;
				if (pe.from->falseSucc == c0) pe.from->falseSucc = combined;
			}
			if (combined->trueSucc) {
				for (auto& pe : combined->trueSucc->preds) {
					if (pe.from == c1 || pe.from == c2) pe.from = combined;
				}
				deduplicate_preds(combined->trueSucc);
			}
			if (combined->falseSucc) {
				for (auto& pe : combined->falseSucc->preds) {
					if (pe.from == c1 || pe.from == c2) pe.from = combined;
				}
				deduplicate_preds(combined->falseSucc);
			}

			c0->removed = true;
			c1->removed = true;
			c2->removed = true;
			replace_in_condition_nodes(c0, combined);
			remove_from_condition_nodes(c1);
			remove_from_condition_nodes(c2);
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
			combined->preds = root->preds;
			update_pred_references(root, combined);
			update_succ_references(NG, combined);
			root->removed = true;
			NG->removed = true;
			replace_in_condition_nodes(root, combined);
			remove_from_condition_nodes(NG);
			return true;
		}
		if ((NG == trueTarget || NG->isExit()) && !NR->isExit() && !NR->removed) {
			Node* combined = new_node(Node::OR);
			combined->nodeLabel = root->nodeLabel;
			combined->leftNode = copy_node_shallow(root);
			combined->rightNode = copy_node_shallow(NR);
			combined->trueSucc = NR->trueSucc ? NR->trueSucc : trueTarget;
			combined->falseSucc = NR->falseSucc;
			combined->preds = root->preds;
			update_pred_references(root, combined);
			update_succ_references(NR, combined);
			root->removed = true;
			NR->removed = true;
			replace_in_condition_nodes(root, combined);
			remove_from_condition_nodes(NR);
			return true;
		}
		return false;
	}

	Expression* build_chain_expression(Node* root, Node* anchor, bool isAnd) {
		std::vector<Node*> chain;
		for (auto node : topo) {
			if (node->removed || node->isExit()) continue;
			if (node->topoIndex >= root->topoIndex && node->topoIndex <= anchor->topoIndex) chain.push_back(node);
		}
		if (!chain.size()) return nullptr;
		if (chain.size() == 1) return build_expression(chain[0]);
		return build_expression_recursive(chain, 0, (uint32_t)chain.size() - 1, isAnd);
	}

		 Expression* build_expression_recursive(const std::vector<Node*>& chain, uint32_t start, uint32_t end, bool isAnd) {
		if (start > end) return nullptr;
		if (start == end) return build_expression(chain[start]);
		if (end - start == 1) {
			return build_binary(isAnd ? Node::AND : Node::OR, build_expression(chain[start]), build_expression(chain[end]));
		}
		
		Node* first = chain[start];
		Node* second = chain[start + 1];
		
		bool greenSkips = first->trueSucc && !first->trueSucc->isExit() && first->trueSucc->topoIndex > second->topoIndex;
		bool redSkips = first->falseSucc && !first->falseSucc->isExit() && first->falseSucc->topoIndex > second->topoIndex;
		
		if (greenSkips && !redSkips) {
			uint32_t splitPoint = end;
			for (uint32_t i = start + 1; i <= end; i++) {
				if (chain[i] == first->trueSucc || chain[i]->topoIndex >= first->trueSucc->topoIndex) {
					splitPoint = i;
					break;
				}
			}
			// Only split if we found the target strictly before the end of the chain
			if (splitPoint < end) {
				Expression* left = build_expression_recursive(chain, start, splitPoint, false);
				Expression* right = build_expression_recursive(chain, splitPoint + 1, end, isAnd);
				return build_binary(Node::AND, left, right);
			}
		}
		
		if (redSkips && !greenSkips) {
			uint32_t splitPoint = end;
			for (uint32_t i = start + 1; i <= end; i++) {
				if (chain[i] == first->falseSucc || chain[i]->topoIndex >= first->falseSucc->topoIndex) {
					splitPoint = i;
					break;
				}
			}
			// Only split if we found the target strictly before the end of the chain
			if (splitPoint < end) {
				Expression* left = build_expression_recursive(chain, start, splitPoint, true);
				Expression* right = build_expression_recursive(chain, splitPoint + 1, end, isAnd);
				return build_binary(Node::OR, left, right);
			}
		}
		
		// Fallback: Default sequential combination (guarantees forward progress)
		return build_binary(isAnd ? Node::AND : Node::OR, build_expression(chain[start]), build_expression_recursive(chain, start + 1, end, isAnd));
	 }

	Node* create_combined_node(Node* root, Node* anchor, Expression* expr, Node::TYPE opType) {
		Node* combined = new_node(opType);
		combined->nodeLabel = root->nodeLabel;
		combined->trueSucc = anchor->trueSucc;
		combined->falseSucc = anchor->falseSucc;
		combined->preds = root->preds;
		combined->resultExpression = expr;
		update_pred_references(root, combined);
		update_succ_references(anchor, combined);
		for (auto node : topo) {
			if (node->removed || node->isExit()) continue;
			if (node->topoIndex >= root->topoIndex && node->topoIndex <= anchor->topoIndex && node != root) node->removed = true;
		}
		root->removed = true;
		replace_in_condition_nodes(root, combined);
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
		for (uint32_t i = 0; i < conditionNodes.size(); i++) {
			if (conditionNodes[i] == oldNode) {
				conditionNodes[i] = newNode;
				return;
			}
		}
	}

	void remove_from_condition_nodes(Node* node) {
		conditionNodes.erase(
			std::remove(conditionNodes.begin(), conditionNodes.end(), node),
			conditionNodes.end());
	}

	bool reduce_patterns() {
		bool changed = true;
		while (changed) {
			changed = false;
			if (reduce_ternary()) {
				changed = true;
				continue;
			}
			if (reduce_and_or()) {
				changed = true;
				continue;
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
			return node->inverted ? build_not((*node->expressions).back()) : (*node->expressions).back();
		case Node::FALSY_TEST:
			return node->inverted ? (*node->expressions).back() : build_not((*node->expressions).back());
		case Node::BOOL_TRUTHY_TEST:
			return node->inverted ? build_not((*node->expressions).back()) : build_not(build_not((*node->expressions).back()));
		case Node::BOOL_FALSY_TEST:
			return node->inverted ? build_not(build_not((*node->expressions).back())) : build_not((*node->expressions).back());
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
			throw nullptr;
		}
	}

	Expression* build_not(Expression* const& operand) {
		Expression* const expression = ast.new_expression(AST_EXPRESSION_UNARY_OPERATION);
		expression->unaryOperation->type = AST_UNARY_NOT;
		expression->unaryOperation->operand = operand;
		return expression;
	}

	Expression* build_binary(const Node::TYPE& type, Expression* const& leftOperand, Expression* const& rightOperand) {
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
			throw nullptr;
		}
		expression->binaryOperation->leftOperand = leftOperand;
		expression->binaryOperation->rightOperand = rightOperand;
		return expression;
	}

	Expression* build_condition() {
		if (!link_nodes()) return nullptr;
		if (!topological_sort()) return nullptr;
		if (!make_monochromatic()) return nullptr;
		if (!reduce_patterns()) return nullptr;
		if (conditionNodes.empty()) return nullptr;
		Node* node = conditionNodes.back();
		if (type == STATEMENT && node->trueSucc == falseTarget) {
			if (node->resultExpression) {
				node->resultExpression = build_not(node->resultExpression);   // <-- ADDED
			} else {
				node->inverted = !node->inverted;
			}
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
};
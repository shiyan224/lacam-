#include "../include/planner.hpp"

LNode::LNode(LNode* _parent, uint i, Vertex* v)
    : who(i), where(v), parent(_parent), depth(parent == nullptr ? 0 : parent->depth + 1)
{
//  if (parent != nullptr) { // 内存有优化空间
//    who = parent->who;
//    who.push_back(i);
//    where = parent->where;
//    where.push_back(v);
//  }
}

uint HNode::HNODE_CNT = 0;

// for high-level, 构造函数，生成节点时从父亲继承、更新每个agent的优先级
HNode::HNode(const Config& _C, DistTable& D, HNode* _parent, const uint _g,
             const uint _h)
    : C(_C),
      parent(_parent),
      neighbor(),
      g(_g),
      h(_h),
      f(g + h),
      priorities(C.size()),
      order(C.size(), 0),
      search_tree(std::queue<LNode*>())
{
  ++HNODE_CNT;

  search_tree.push(new LNode());
  const auto N = C.size();

  // update neighbor
  if (parent != nullptr) parent->neighbor.insert(this);

  // set priorities
  if (parent == nullptr) {
    // initialize
    for (uint i = 0; i < N; ++i) priorities[i] = (float)D.get(i, C[i]) / N;
  } else {
    // dynamic priorities, akin to PIBT
    for (size_t i = 0; i < N; ++i) {
      if (D.get(i, C[i]) != 0) {
        priorities[i] = parent->priorities[i] + 1;
      } else {
        priorities[i] = parent->priorities[i] - (int)parent->priorities[i];
      }
    }
  }

  // set order，按照priority从高到底决定agent的优先级
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](uint i, uint j) { return priorities[i] > priorities[j]; });
}

HNode::~HNode()
{
  while (!search_tree.empty()) {
    delete search_tree.front();
    search_tree.pop();
  }
}

Planner::Planner(const Instance* _ins, const Deadline* _deadline,
                 std::mt19937* _MT, const int _verbose,
                 const Objective _objective, const float _restart_rate)
    : ins(_ins),
      deadline(_deadline),
      MT(_MT),
      verbose(_verbose),
      objective(_objective),
      RESTART_RATE(_restart_rate),
      N(ins->N),
      V_size(ins->G.size()),
      D(DistTable(ins)),
      loop_cnt(0),
      C_next(N),
      tie_breakers(V_size, 0),
      A(N, nullptr),
      occupied_now(V_size, nullptr),
      occupied_next(V_size, nullptr)
{
}

Planner::~Planner() {}

Solution Planner::solve(std::string& additional_info)
{
  solver_info(1, "start search");

  // setup agents
  for (auto i = 0; i < N; ++i) A[i] = new Agent(i);

  // setup search
  auto OPEN = std::stack<HNode*>();
  auto EXPLORED = std::unordered_map<Config, HNode*, ConfigHasher>();
  // insert initial node, 'H': high-level node
  auto H_init = new HNode(ins->starts, D, nullptr, 0, get_h_value(ins->starts));
  OPEN.push(H_init);
  EXPLORED[H_init->C] = H_init;

  std::vector<Config> solution;
  auto C_new = Config(N, nullptr);  // for new configuration
  HNode* H_goal = nullptr;          // to store goal node

  // DFS
  while (!OPEN.empty() && !is_expired(deadline)) {
    loop_cnt += 1;

    // do not pop here!
    auto H = OPEN.top();  // high-level node

    // low-level search end
    if (H->search_tree.empty()) {
      OPEN.pop();
      continue;
    }

    // check lower bounds
    if (H_goal != nullptr && H->f >= H_goal->f) { // 从当前节点的后继节点中不可能得到更好的结果，直接pop，剪枝
      OPEN.pop();
      continue;
    }

    // check goal condition 所有agent到达终点
    if (H_goal == nullptr && is_same_config(H->C, ins->goals)) {
      H_goal = H;
      solver_info(1, "found solution, cost: ", H->g);
      //if (objective == OBJ_NONE) break;
//      continue;
      break;
    }

    // create successors at the low-level search, BFS
    auto L = H->search_tree.front();
    H->search_tree.pop();
    expand_lowlevel_tree(H, L);

    // create successors at the high-level search
    const auto res = get_new_config(H, L);
    //delete L;  // free
    if (!res) continue;

    // create new configuration
    for (auto a : A) C_new[a->id] = a->v_next;

    // check explored list
    const auto iter = EXPLORED.find(C_new);
    if (iter != EXPLORED.end()) { // C_new出现过，更新
      // case found
      rewrite(H, iter->second, H_goal,OPEN); // dijkstra
      // re-insert or random-restart

      auto H_insert = (MT != nullptr && get_random_float(MT) >= RESTART_RATE)
                          ? iter->second
                          : H_init;
      if (H_goal == nullptr || H_insert->f < H_goal->f) OPEN.push(H_insert);
    } else {
      // insert new search node
      const auto H_new = new HNode(
          C_new, D, H, H->g + get_edge_cost(H->C, C_new), get_h_value(C_new));
      EXPLORED[H_new->C] = H_new;
      if (H_goal == nullptr || H_new->f < H_goal->f) OPEN.push(H_new);
    }
  }

  // backtrack
  if (H_goal != nullptr) {
    auto H = H_goal;
    while (H != nullptr) {
      solution.push_back(H->C);
      H = H->parent;
    }
    std::reverse(solution.begin(), solution.end());
  }

  // print result
  if (H_goal != nullptr && OPEN.empty()) {
    solver_info(1, "solved optimally, objective: ", objective);
  } else if (H_goal != nullptr) {
    solver_info(1, "solved sub-optimally, objective: ", objective);
  } else if (OPEN.empty()) {
    solver_info(1, "no solution");
  } else {
    solver_info(1, "timeout");
  }

  // logging
  additional_info +=
      "optimal=" + std::to_string(H_goal != nullptr && OPEN.empty()) + "\n";
  additional_info += "objective=" + std::to_string(objective) + "\n";
  additional_info += "loop_cnt=" + std::to_string(loop_cnt) + "\n";
  additional_info += "num_node_gen=" + std::to_string(EXPLORED.size()) + "\n";

  // memory management
  for (auto a : A) delete a;
  for (auto itr : EXPLORED) delete itr.second;

  return solution;
}

void Planner::rewrite(HNode* H_from, HNode* H_to, HNode* H_goal,
                      std::stack<HNode*>& OPEN)
{
  // update neighbors
  H_from->neighbor.insert(H_to);

  // Dijkstra update
  std::queue<HNode*> Q({H_from});  // queue is sufficient
  while (!Q.empty()) {
    auto n_from = Q.front();
    Q.pop();
    for (auto n_to : n_from->neighbor) {
      auto g_val = n_from->g + get_edge_cost(n_from->C, n_to->C);
      if (g_val < n_to->g) {
        if (n_to == H_goal)
          solver_info(1, "cost update: ", n_to->g, " -> ", g_val);
        uint _n_to = n_to->f;
        n_to->g = g_val;
        n_to->f = n_to->g + n_to->h;
        n_to->parent = n_from;
        Q.push(n_to);
        if (H_goal != nullptr && n_to->f < H_goal->f)
          OPEN.push(n_to); // 这条路原先可能走不通，现在能走通了 && _n_to >= H_goal->f
      }
    }
  }
}

uint Planner::get_edge_cost(const Config& C1, const Config& C2)
{
  if (objective == OBJ_NONE) {
    uint cost = 0;
    for (uint i = 0; i < N; ++i) {
      if (C1[i] != C2[i])
        cost += 1;
    }
    return cost;
  }
  if (objective == OBJ_SUM_OF_LOSS) {
    uint cost = 0;
    for (uint i = 0; i < N; ++i) {
      if (C1[i] != ins->goals[i] || C2[i] != ins->goals[i]) {
        cost += 1;
      }
    }
    return cost;
  }

  // default: makespan
  return 1;
}

uint Planner::get_edge_cost(HNode* H_from, HNode* H_to)
{
  return get_edge_cost(H_from->C, H_to->C);
}

uint Planner::get_h_value(const Config& C)
{
  uint cost = 0;
  if (objective == OBJ_MAKESPAN) {
    for (auto i = 0; i < N; ++i) cost = std::max(cost, D.get(i, C[i]));
  } else {  //if (objective == OBJ_SUM_OF_LOSS)
      for (auto i = 0; i < N; ++i) cost += D.get(i, C[i]);
  }
  return cost;
}

void Planner::expand_lowlevel_tree(HNode* H, LNode* L)
{
  if (L->depth >= N) return;
  const auto i = H->order[L->depth];
  auto C = H->C[i]->neighbor;
  C.push_back(H->C[i]);
  // randomize
  if (MT != nullptr) std::shuffle(C.begin(), C.end(), *MT);
  // insert
  for (auto v : C) H->search_tree.push(new LNode(L, i, v));
}

bool Planner::get_new_config(HNode* H, LNode* L)
{
  // setup cache
  for (auto a : A) {
    // clear previous cache
    if (a->v_now != nullptr && occupied_now[a->v_now->id] == a) {
      occupied_now[a->v_now->id] = nullptr;  // 高效初始化
    }
    if (a->v_next != nullptr) {
      occupied_next[a->v_next->id] = nullptr;
      a->v_next = nullptr;
    }

    // set occupied now
    a->v_now = H->C[a->id];
    occupied_now[a->v_now->id] = a;
  }

  // add constraints
  LNode* tmp = L;
  for (uint k = 0; k < L->depth; ++k) {
    const auto i = tmp->who;        // agent
    const auto l = tmp->where->id;  // loc

    // check vertex collision
    if (occupied_next[l] != nullptr) return false;
    // check swap collision
    auto l_pre = H->C[i]->id;
    if (occupied_next[l_pre] != nullptr && occupied_now[l] != nullptr &&
        occupied_next[l_pre]->id == occupied_now[l]->id)
      return false;

    // set occupied_next
    A[i]->v_next = tmp->where;
    occupied_next[l] = A[i];
    tmp = tmp -> parent;
  }

  // perform PIBT
  for (auto k : H->order) {
    auto a = A[k];
    if (a->v_next == nullptr && !funcPIBT(a)) return false;  // planning failure
  }
  return true;
}

//float Planner::h(uint i, Vertex* v, HNode* H)
//{
//  float ret = 0;
//  auto aj = occupied_now[v->id];
//  if (aj != nullptr) {
//    if (aj == A[i])
//      return 0;
//
//    if (D.get(aj->id, aj->v_now) == 0)
//      ret += 0.2;
//  }
//  return ret;
//}

bool Planner::funcPIBT(Agent* ai) // PIBT*
{
  const auto i = ai->id;
  const auto K = ai->v_now->neighbor.size();

  // get candidates for next locations
  for (auto k = 0; k < K; ++k) {
    auto u = ai->v_now->neighbor[k];
    C_next[i][k] = u;
    if (MT != nullptr)
      tie_breakers[u->id] = get_random_float(MT);  // set tie-breaker
//    auto aj = occupied_now[u->id];
//    if (aj != nullptr && D.get(aj->id, u) == 0 && aj != ai)
//        tie_breakers[u->id] += 0.5;
  }
  C_next[i][K] = ai->v_now;
//  tie_breakers[ai->v_now->id] = get_random_float(MT, 0, 0.49);
  // sort
  std::sort(C_next[i].begin(), C_next[i].begin() + K + 1,
            [&](Vertex* const v, Vertex* const u) {
              return (float)D.get(i, v)  + tie_breakers[v->id] <
                     (float)D.get(i, u)  +tie_breakers[u->id];
            });

  Agent* swap_agent = nullptr;
  if (FLG_SWAP) {
    swap_agent = swap_possible_and_required(ai);
    if (swap_agent != nullptr)
      std::reverse(C_next[i].begin(), C_next[i].begin() + K + 1);
  }

  // main operation
  for (auto k = 0; k < K + 1; ++k) {
    auto u = C_next[i][k]; // 备用节点

    // avoid vertex conflicts
    if (occupied_next[u->id] != nullptr) continue; // 节点u下一时刻将被占据

    auto& ak = occupied_now[u->id]; // 选取当前占据u节点的agent

    // avoid swap conflicts
    if (ak != nullptr && ak->v_next == ai->v_now) continue; // 如果该agent下一时刻要来到当前位置，swap conflict

    // reserve next location
    occupied_next[u->id] = ai; // 不会发生任何冲突
    ai->v_next = u;

    // priority inheritance
    if (ak != nullptr && ak != ai && ak->v_next == nullptr && !funcPIBT(ak))
      continue;

    // success to plan next one step
    // pull swap_agent when applicable
    if (FLG_SWAP) {
      if (k == 0 && swap_agent != nullptr && swap_agent->v_next == nullptr &&
          occupied_next[ai->v_now->id] == nullptr) {
        swap_agent->v_next = ai->v_now;
        occupied_next[swap_agent->v_next->id] = swap_agent;
      }
    }
    return true;
  }

  // failed to secure node
  occupied_next[ai->v_now->id] = ai; // why? 停留原地的选项不是也已经进行过尝试了吗？
  ai->v_next = ai->v_now;
  return false;
}

Agent* Planner::swap_possible_and_required(Agent* ai)
{
  const auto i = ai->id;
  // ai wanna stay at v_now -> no need to swap
  if (C_next[i][0] == ai->v_now) return nullptr;

  // usual swap situation, c.f., case-a, b
  auto aj = occupied_now[C_next[i][0]->id];
  if (aj != nullptr && aj->v_next == nullptr &&
      is_swap_required(ai->id, aj->id, ai->v_now, aj->v_now) &&
      is_swap_possible(aj->v_now, ai->v_now)) {
    return aj;
  }

  // for clear operation, c.f., case-c 防止重复吧
  for (auto u : ai->v_now->neighbor) { // 遍历ai的邻居
    auto ak = occupied_now[u->id]; //
    if (ak == nullptr || C_next[i][0] == ak->v_now) continue; // 如果该位置上没有agent，或者ak在ai的最短路径上
    if (is_swap_required(ak->id, ai->id, ai->v_now, C_next[i][0]) &&
        is_swap_possible(C_next[i][0], ai->v_now)) {
      return ak;
    }
  }

  return nullptr;
}

// simulate whether the swap is required
bool Planner::is_swap_required(const uint pusher, const uint puller,
                               Vertex* v_pusher_origin, Vertex* v_puller_origin)
{
  auto v_pusher = v_pusher_origin;
  auto v_puller = v_puller_origin;
  Vertex* tmp = nullptr;
  while (D.get(pusher, v_puller) < D.get(pusher, v_pusher)) { // puller在pusher的路径上
    auto n = v_puller->neighbor.size();
    // remove agents who need not to move
    for (auto u : v_puller->neighbor) {
      auto a = occupied_now[u->id];
      if (u == v_pusher ||
          (u->neighbor.size() == 1 && a != nullptr && ins->goals[a->id] == u)) {
        --n;
      } else {
        tmp = u;
      }
    }
    if (n >= 2) return false;  // able to swap
    if (n <= 0) break;
    v_pusher = v_puller;
    v_puller = tmp;
  }
  //推到不能推（puller无路可走） or 无需再推（pusher到达goal）
  // judge based on distance
  return (D.get(puller, v_pusher) < D.get(puller, v_puller)) && // pusher在puller的路径上
         (D.get(pusher, v_pusher) == 0 ||
          D.get(pusher, v_puller) < D.get(pusher, v_pusher)); // pusher到达goal 或 puller仍在pusher的路径上
}

// simulate whether the swap is possible
bool Planner::is_swap_possible(Vertex* v_pusher_origin, Vertex* v_puller_origin)
{
  auto v_pusher = v_pusher_origin;
  auto v_puller = v_puller_origin;
  Vertex* tmp = nullptr;
  while (v_puller != v_pusher_origin) {  // avoid loop
    auto n = v_puller->neighbor.size();  // count #(possible locations) to pull
    for (auto u : v_puller->neighbor) {
      auto a = occupied_now[u->id];
      if (u == v_pusher ||
          (u->neighbor.size() == 1 && a != nullptr && ins->goals[a->id] == u)) {
        --n;      // pull-impossible with u
      } else {
        tmp = u;  // pull-possible with u
      }
    }
    if (n >= 2) return true;  // able to swap
    if (n <= 0) return false;
    v_pusher = v_puller;
    v_puller = tmp;
  }
  return false;
}

std::ostream& operator<<(std::ostream& os, const Objective obj)
{
  if (obj == OBJ_NONE) {
    os << "none";
  } else if (obj == OBJ_MAKESPAN) {
    os << "makespan";
  } else if (obj == OBJ_SUM_OF_LOSS) {
    os << "sum_of_loss";
  }
  return os;
}

#include "../include/planner.hpp"

#include <string>

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
      OPEN(std::stack<Node*>()),
      CLOSED(std::unordered_map<Config, Node*, ConfigHasher>()),
      S_goal(nullptr),
      loop_cnt(0),
      C_next(Candidates(N, std::array<Vertex*, 5>())),
      tie_breakers(std::vector<float>(V_size, 0)),
      A(Agents(N, nullptr)),
      occupied_now(Agents(V_size, nullptr)),
      occupied_next(Agents(V_size, nullptr))
{
  // setup agents
  for (uint i = 0; i < N; ++i) A[i] = new Agent(i);
}

Planner::~Planner()
{
  // memory management
  for (auto a : A) delete a;
  for (auto p : CLOSED) delete p.second;
}

Solution Planner::solve(std::string& additional_info)
{
  solver_info(1, "start search");

  // insert initial node
  auto S_init = new Node(ins->starts, D, nullptr, 0, get_h_value(ins->starts));
  OPEN.push(S_init);
  CLOSED[S_init->C] = S_init;

  // BFS
  std::vector<Config> solution;
  auto C_new = Config(N, nullptr);  // new configuration

  while (!OPEN.empty() && !is_expired(deadline)) {
    loop_cnt += 1;

    // do not pop here!
    auto S = OPEN.top();

    // low-level search end
    if (S->search_tree.empty()) {
      OPEN.pop();
      continue;
    }

    // // check lower bounds
    // if (S_goal != nullptr && S->f >= S_goal->f) {
    //   OPEN.pop();
    //   continue;
    // }

    // check goal condition
    if (S_goal == nullptr && is_same_config(S->C, ins->goals)) {
      S_goal = S;
      solver_info(1, "found solution, cost: ", S->g);
      update_hist();
      if (objective == OBJ_NONE) break;
      continue;
    }

    // create successors at the low-level search
    auto M = S->search_tree.front();
    S->search_tree.pop();
    expand_lowlevel_tree(S, M);

    // create successors at the high-level search
    const auto res = get_new_config(S, M);
    delete M;  // free
    if (!res) continue;

    // create new configuration
    for (auto a : A) C_new[a->id] = a->v_next;

    // check explored list
    const auto iter = CLOSED.find(C_new);
    if (iter != CLOSED.end()) {
      // case found
      rewrite(S, iter->second);
      // re-insert or random-restart
      auto T = get_random_float(MT) >= RESTART_RATE ? iter->second : S_init;
      if (S_goal == nullptr || T->f < S_goal->f) OPEN.push(T);
    } else {
      // insert new search node
      const auto S_new = new Node(
          C_new, D, S, S->g + get_edge_cost(S->C, C_new), get_h_value(C_new));
      CLOSED[S_new->C] = S_new;
      if (S_goal == nullptr || S_new->f < S_goal->f) OPEN.push(S_new);
    }
  }

  // backtrack
  if (S_goal != nullptr) {
    auto S = S_goal;
    while (S != nullptr) {
      solution.push_back(S->C);
      S = S->parent;
    }
    std::reverse(solution.begin(), solution.end());
  }

  if (S_goal != nullptr && OPEN.empty()) {
    solver_info(1, "solved optimally, objective: ", objective);
  } else if (S_goal != nullptr) {
    solver_info(1, "solved sub-optimally, objective: ", objective);
  } else if (OPEN.empty()) {
    solver_info(1, "no solution");
  } else {
    solver_info(1, "timeout");
  }

  // logging
  additional_info +=
      "optimal=" + std::to_string(S_goal != nullptr && OPEN.empty()) + "\n";
  additional_info += "objective=" + std::to_string(objective) + "\n";
  additional_info += "loop_cnt=" + std::to_string(loop_cnt) + "\n";
  additional_info += "num_node_gen=" + std::to_string(CLOSED.size()) + "\n";
  update_hist();
  additional_info += "hist_cost=";
  for (auto c : hist_cost) additional_info += std::to_string(c) + ",";
  additional_info += "\nhist_time=";
  for (auto c : hist_time) additional_info += std::to_string(c) + ",";
  additional_info += "\n";

  return solution;
}

void Planner::rewrite(Node* S, Node* T)
{
  S->neighbor[T->id] = T;
  T->neighbor[S->id] = S;  // since the graph is undirected
  auto c = S->g + get_edge_cost(S, T);
  if (c >= T->g) return;  // no need to update costs

  // update neighbors
  // in this implementation, BFS is sufficient rather than Dijkstra
  std::queue<Node*> Q;
  Q.push(S);
  while (!Q.empty()) {
    auto U = Q.front();
    Q.pop();
    for (auto iter : U->neighbor) {
      auto W = iter.second;
      if (U == S && W != T) continue;  // skip redundant check
      auto c = U->g + get_edge_cost(U, W);
      if (c < W->g) {
        if (W == S_goal) solver_info(1, "cost update: ", W->g, " -> ", c);
        W->g = c;
        W->f = W->g + W->h;
        W->parent = U;
        Q.push(W);
        if (W == S_goal) update_hist();
        // if (S_goal != nullptr && W->f < S_goal->f) OPEN.push(W);
      }
    }
  }
}

uint Planner::get_edge_cost(const Config& C1, const Config& C2)
{
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

uint Planner::get_edge_cost(Node* S, Node* T)
{
  return get_edge_cost(S->C, T->C);
}

uint Planner::get_h_value(const Config& C)
{
  uint cost = 0;
  if (objective == OBJ_MAKESPAN) {
    for (size_t i = 0; i < N; ++i) cost = std::max(cost, D.get(i, C[i]));
  } else if (objective == OBJ_SUM_OF_LOSS) {
    for (size_t i = 0; i < N; ++i) cost += D.get(i, C[i]);
  }
  return cost;
}

void Planner::expand_lowlevel_tree(Node* S, Constraint* M)
{
  if (M->depth >= N) return;
  const auto i = S->order[M->depth];
  auto C = S->C[i]->neighbor;
  C.push_back(S->C[i]);
  // randomize
  if (MT != nullptr) std::shuffle(C.begin(), C.end(), *MT);
  // insert
  for (auto v : C) S->search_tree.push(new Constraint(M, i, v));
}

void Planner::update_hist()
{
  if (S_goal == nullptr) return;
  hist_cost.push_back(S_goal->g);
  hist_time.push_back(elapsed_ms(deadline));
}

bool Planner::get_new_config(Node* S, Constraint* M)
{
  // setup cache
  for (auto a : A) {
    // clear previous cache
    if (a->v_now != nullptr && occupied_now[a->v_now->id] == a) {
      occupied_now[a->v_now->id] = nullptr;
    }
    if (a->v_next != nullptr) {
      occupied_next[a->v_next->id] = nullptr;
      a->v_next = nullptr;
    }

    // set occupied now
    a->v_now = S->C[a->id];
    occupied_now[a->v_now->id] = a;
  }

  // add constraints
  for (uint k = 0; k < M->depth; ++k) {
    const auto i = M->who[k];        // agent
    const auto l = M->where[k]->id;  // loc

    // check vertex collision
    if (occupied_next[l] != nullptr) return false;
    // check swap collision
    auto l_pre = S->C[i]->id;
    if (occupied_next[l_pre] != nullptr && occupied_now[l] != nullptr &&
        occupied_next[l_pre]->id == occupied_now[l]->id)
      return false;

    // set occupied_next
    A[i]->v_next = M->where[k];
    occupied_next[l] = A[i];
  }

  // perform PIBT
  for (auto k : S->order) {
    auto a = A[k];
    if (a->v_next == nullptr && !funcPIBT(a)) return false;  // planning failure
  }
  return true;
}

bool Planner::funcPIBT(Agent* ai, Agent* aj)
{
  const auto i = ai->id;
  const auto K = ai->v_now->neighbor.size();

  // get candidates for next locations
  for (size_t k = 0; k < K; ++k) {
    auto u = ai->v_now->neighbor[k];
    C_next[i][k] = u;
    if (MT != nullptr)
      tie_breakers[u->id] = get_random_float(MT);  // set tie-breaker
  }
  C_next[i][K] = ai->v_now;

  // sort
  std::sort(C_next[i].begin(), C_next[i].begin() + K + 1,
            [&](Vertex* const v, Vertex* const u) {
              return D.get(i, v) + tie_breakers[v->id] <
                     D.get(i, u) + tie_breakers[u->id];
            });

  // main operation
  for (size_t k = 0; k < K + 1; ++k) {
    auto u = C_next[i][k];

    // avoid vertex conflicts
    if (occupied_next[u->id] != nullptr) continue;
    // avoid swap conflicts
    if (aj != nullptr && u == aj->v_now) continue;

    auto& ak = occupied_now[u->id];

    // avoid swap confilicts with constraints
    if (ak != nullptr && ak->v_next == ai->v_now) continue;

    // reserve next location
    occupied_next[u->id] = ai;
    ai->v_next = u;

    // empty or stay
    if (ak == nullptr || u == ai->v_now) {
      return true;
    }

    // priority inheritance
    if (ak->v_next == nullptr && !funcPIBT(ak, ai)) continue;

    // success to plan next one step
    return true;
  }

  // failed to secure node
  occupied_next[ai->v_now->id] = ai;
  ai->v_next = ai->v_now;
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

Solution solve(const Instance& ins, std::string& additional_info,
               const int verbose, const Deadline* deadline, std::mt19937* MT,
               const Objective objective, const float restart_rate)
{
  auto planner = Planner(&ins, deadline, MT, verbose, objective, restart_rate);
  return planner.solve(additional_info);
}

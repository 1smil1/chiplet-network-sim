#include <algorithm>
#include <set>

#include "railx_2d_hyperx.h"

std::vector<std::vector<int>> gen_rc_square(int n) {
  assert(n % 2 == 0 && n > 0 && "n must be positive and even");

  std::vector<int> l0(n);
  for (int i = 0; i < n; ++i) {
    if (i % 2 == 0 && i > 0)
      l0[i] = n - i / 2;
    else
      l0[i] = (i + 1) / 2;
  }

  std::set<int> checkSet(l0.begin(), l0.end());
  assert(checkSet.size() == n && *checkSet.rbegin() == n - 1 && *checkSet.begin() == 0);

  std::vector<std::vector<int>> l;
  l.push_back(l0);
  for (int i = 0; i < n - 1; ++i) {
    std::vector<int> l_next(n);
    for (int j = 0; j < n; ++j) {
      l_next[j] = (l.back()[j] + 1) % n;
    }
    l.push_back(l_next);
  }
  return l;
}

std::vector<std::vector<int>> gen_hamilton_decomp_odd(int n) {
  assert(n % 2 == 1 && n > 1 && "Need \"n = 2m+1, m > 0\"!");
  std::vector<std::vector<int>> l = gen_rc_square(n - 1);  // n-1 = 2m
  std::vector<std::vector<int>> hcycle;
  for (int i = 0; i < n - 1; ++i) {
    std::vector<int> path = l[i];
    path.push_back(n - 1);
    hcycle.push_back(path);
  }
  assert(check_hamilton_decomp(hcycle, n) &&
         "The produced hcycle does not form a Hamiltonian cycle!");
  return hcycle;
}

std::vector<std::vector<int>> gen_hamilton_decomp_4(int n) {
  assert(n % 4 == 0 && n > 4 && "Need \"n = 4m, m > 1\"!");

  std::vector<std::vector<int>> l = gen_rc_square(n - 2);  // n-2 = 4k+2
  int k = n / 4 - 1;
  std::vector<std::vector<int>> hcycle;
  std::vector<int> other_edges(n - 1, -1);

  for (int i = 0; i < n - 2; ++i) {
    std::vector<int> cur_l = l[i];
    cur_l.push_back(n - 2);  // len = 4k+3
    int v = cur_l[2 * k - 1];
    int div_pnt;

    if (v == k) {
      assert(i == 0);
      div_pnt = 0;
    } else if (v == 2 * k + 1) {
      div_pnt = 4 * k + 1;
    } else if (v == 3 * k + 2) {
      div_pnt = 2;
    } else if (v == 0) {
      div_pnt = 4 * k - 1;
    } else {
      div_pnt = 2 * k - 1;
    }

    assert(div_pnt >= 0 && div_pnt < cur_l.size());
    rotate(cur_l.begin(), cur_l.begin() + div_pnt, cur_l.end());

    int a = cur_l.back(), b = cur_l.front();
    cur_l.push_back(n - 1);
    hcycle.push_back(cur_l);

    assert(other_edges[a] == -1 && "The deleted edges do not form a Hamiltonian path!");
    other_edges[a] = b;
  }

  std::set<int> s;
  for (int i = 0; i < n - 1; ++i) {
    s.insert(i);
  }
  for (auto edge : other_edges) {
    if (edge != -1) s.erase(edge);
  }

  assert(s.size() == 1);
  int a = *s.begin();
  std::vector<int> cur_l;

  while (a != -1) {
    cur_l.push_back(a);
    a = other_edges[a];
  }

  assert(cur_l.size() == n - 1 && "The deleted edges do not form a Hamiltonian path!");
  cur_l.push_back(n - 1);
  hcycle.push_back(cur_l);

  assert(check_hamilton_decomp(hcycle, n) &&
         "The produced hcycle does not form a Hamiltonian cycle!");
  return hcycle;
}

int in_which_cycle(std::pair<int, int> link, const std::vector<std::vector<int>>& hamilton_decomp) {
  for (int i = 0; i < hamilton_decomp.size(); i++) {
    const std::vector<int>& cycle = hamilton_decomp[i];
    for (int j = 0; j < cycle.size(); ++j) {
      if (cycle[j] == link.first && cycle[(j + 1) % cycle.size()] == link.second) {
        return i;
      }
    }
  }
  assert(false && "Invalid link in the Hamiltonian decomposition!");
}

bool check_hamilton_decomp(const std::vector<std::vector<int>>& hcycle, int n) {
  std::set<std::pair<int, int>> edge_set;
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      if (i != j) {
        edge_set.insert({i, j});
      }
    }
  }

  for (const auto& cycle : hcycle) {
    std::set<int> cycle_set(cycle.begin(), cycle.end());
    assert(cycle_set.size() == n && "The produced hcycle does not form a Hamiltonian cycle!");

    for (size_t j = 0; j < cycle.size(); ++j) {
      int u = cycle[j], v = cycle[(j + 1) % cycle.size()];
      assert(edge_set.erase({u, v}) > 0 &&
             "Invalid edge in the Hamiltonian cycle!");
    }
  }
  return true;
}

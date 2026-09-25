// QoR regression check: compiles every design and compares gates/depth/flops
// against tests/golden/qor.txt. Output is deterministic, so any change is either
// a regression (fails) or an improvement (fails too, until the golden file is
// refreshed with --update). That way QoR never changes by accident.

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>

#include "ast.h"
#include "elaborate.h"
#include "flow.h"

using namespace lf;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: qor_regress <designs dir> <golden file> [--update]\n");
    return 1;
  }
  std::string dir = argv[1], golden = argv[2];
  bool update = argc > 3 && std::string(argv[3]) == "--update";

  std::vector<std::string> names;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 3 && n.substr(n.size() - 3) == ".sv") names.push_back(n);
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());

  std::map<std::string, std::string> now;
  for (const auto& n : names) {
    Graph elab = elaborate(parse_file(dir + "/" + n).back());
    FlowOptions o;
    o.threads = 2;
    Qor q = measure(optimize_design(elab, o));
    std::ostringstream os;
    os << "gates=" << q.gates << " area=" << q.area << " depth=" << q.depth << " flops=" << q.flops;
    now[n] = os.str();
  }

  if (update) {
    std::ofstream f(golden);
    for (const auto& [n, v] : now) f << n << " " << v << "\n";
    std::printf("wrote %s\n", golden.c_str());
    return 0;
  }

  std::map<std::string, std::string> want;
  std::ifstream f(golden);
  std::string line;
  while (std::getline(f, line)) {
    auto sp = line.find(' ');
    if (sp != std::string::npos) want[line.substr(0, sp)] = line.substr(sp + 1);
  }
  int bad = 0;
  for (const auto& [n, v] : now) {
    auto it = want.find(n);
    if (it == want.end()) {
      std::printf("NEW  %-14s %s\n", n.c_str(), v.c_str());
      bad++;
    } else if (it->second != v) {
      std::printf("DIFF %-14s golden: %s   now: %s\n", n.c_str(), it->second.c_str(), v.c_str());
      bad++;
    } else {
      std::printf("ok   %-14s %s\n", n.c_str(), v.c_str());
    }
  }
  if (bad) std::printf("%d design(s) changed; if intended, rerun with --update\n", bad);
  return bad ? 1 : 0;
}

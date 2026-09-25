#include "sim.h"

#include <map>
#include <stdexcept>

namespace lf {

std::vector<uint64_t> simulate(const Graph& g, const std::vector<uint64_t>& source_words, size_t words) {
  std::vector<uint64_t> v(g.size() * words, 0);
  for (size_t w = 0; w < words; w++) v[1 * words + w] = ~0ull;
  size_t src = 0;
  for (uint32_t i = 2; i < g.size(); i++) {
    const Node& n = g.nodes[i];
    uint64_t* out = &v[size_t(i) * words];
    if (is_source(n.op)) {
      for (size_t w = 0; w < words; w++) out[w] = source_words[src * words + w];
      src++;
      continue;
    }
    const uint64_t* a = &v[size_t(n.in[0]) * words];
    const uint64_t* b = &v[size_t(n.in[1]) * words];
    const uint64_t* c = &v[size_t(n.in[2]) * words];
    for (size_t w = 0; w < words; w++) {
      switch (n.op) {
        case Op::Not: out[w] = ~a[w]; break;
        case Op::And: out[w] = a[w] & b[w]; break;
        case Op::Or: out[w] = a[w] | b[w]; break;
        case Op::Xor: out[w] = a[w] ^ b[w]; break;
        case Op::Mux: out[w] = (a[w] & b[w]) | (~a[w] & c[w]); break;
        default: break;
      }
    }
  }
  return v;
}

namespace {

std::string key(const std::string& kind, const std::string& name, int bit) {
  return kind + ":" + name + "[" + std::to_string(bit) + "]";
}

uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// source node id -> index into the shared list of free variables
std::map<uint32_t, size_t> source_slots(const Graph& g, const std::map<std::string, size_t>& vars, bool strict) {
  std::map<uint32_t, size_t> slots;
  for (const auto& b : g.inputs) {
    auto it = vars.find(key("in", b.name, b.bit));
    if (it == vars.end()) throw std::runtime_error("input " + b.name + " missing from reference");
    slots[b.node] = it->second;
  }
  for (const auto& f : g.flops) {
    auto it = vars.find(key("ff", f.name, f.bit));
    if (it == vars.end()) {
      if (strict) throw std::runtime_error("flop " + f.name + " missing from reference");
      continue;
    }
    slots[f.q] = it->second;
  }
  return slots;
}

}  // namespace

EquivResult check_equivalence(const Graph& ref, const Graph& impl, size_t random_words, uint64_t seed) {
  EquivResult res;
  std::map<std::string, size_t> vars;
  for (const auto& b : ref.inputs) vars.emplace(key("in", b.name, b.bit), vars.size());
  for (const auto& f : ref.flops) vars.emplace(key("ff", f.name, f.bit), vars.size());

  // what we compare: every output, plus next-state of flops both sides still have
  struct Probe {
    std::string name;
    uint32_t a, b;
  };
  std::vector<Probe> probes;
  std::map<std::string, uint32_t> impl_out;
  for (const auto& o : impl.outputs) impl_out[key("out", o.name, o.bit)] = o.node;
  for (const auto& o : ref.outputs) {
    auto it = impl_out.find(key("out", o.name, o.bit));
    if (it == impl_out.end()) {
      res.equal = false;
      res.detail = "output " + o.name + "[" + std::to_string(o.bit) + "] missing";
      return res;
    }
    probes.push_back({o.name + "[" + std::to_string(o.bit) + "]", o.node, it->second});
  }
  if (impl_out.size() != ref.outputs.size()) {
    res.equal = false;
    res.detail = "output count differs";
    return res;
  }
  std::map<std::string, uint32_t> impl_ff;
  for (const auto& f : impl.flops) impl_ff[key("ff", f.name, f.bit)] = f.d;
  for (const auto& f : ref.flops) {
    auto it = impl_ff.find(key("ff", f.name, f.bit));
    if (it != impl_ff.end()) probes.push_back({f.name + "[" + std::to_string(f.bit) + "].d", f.d, it->second});
  }

  auto ref_slot = source_slots(ref, vars, true);
  auto impl_slot = source_slots(impl, vars, true);

  size_t nv = vars.size();
  res.exhaustive = nv <= 16;
  size_t total_words = res.exhaustive ? (nv <= 6 ? 1 : size_t(1) << (nv - 6)) : random_words;
  res.patterns = res.exhaustive ? (uint64_t(1) << nv) : uint64_t(total_words) * 64;

  const size_t chunk = 8;
  uint64_t rng = seed;
  for (size_t base = 0; base < total_words; base += chunk) {
    size_t words = std::min(chunk, total_words - base);
    std::vector<uint64_t> var_words(nv * words);
    for (size_t k = 0; k < nv; k++)
      for (size_t w = 0; w < words; w++) {
        uint64_t x;
        if (!res.exhaustive) x = splitmix(rng);
        else if (k < 6) {
          static const uint64_t low[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                          0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
          x = low[k];
        } else x = ((base + w) >> (k - 6)) & 1 ? ~0ull : 0;
        var_words[k * words + w] = x;
      }
    auto feed = [&](const Graph& g, const std::map<uint32_t, size_t>& slots) {
      std::vector<uint64_t> src;
      for (uint32_t i = 2; i < g.size(); i++) {
        if (!is_source(g.nodes[i].op)) continue;
        auto it = slots.find(i);
        for (size_t w = 0; w < words; w++) src.push_back(it == slots.end() ? 0 : var_words[it->second * words + w]);
      }
      return simulate(g, src, words);
    };
    auto va = feed(ref, ref_slot);
    auto vb = feed(impl, impl_slot);
    for (const auto& p : probes)
      for (size_t w = 0; w < words; w++) {
        uint64_t diff = va[size_t(p.a) * words + w] ^ vb[size_t(p.b) * words + w];
        if (!diff) continue;
        int bit = __builtin_ctzll(diff);
        res.equal = false;
        res.detail = p.name + " differs (pattern " + std::to_string((base + w) * 64 + bit) + ")";
        if (res.exhaustive && nv <= 16) {
          uint64_t pat = (base + w) * 64 + bit;
          res.detail += ":";
          for (const auto& [name, slot] : vars) res.detail += " " + name + "=" + std::to_string((pat >> slot) & 1);
        }
        return res;
      }
  }
  return res;
}

}  // namespace lf


#ifndef CLAP_TILDE_BPE_TOKENIZER_H
#define CLAP_TILDE_BPE_TOKENIZER_H

// RoBERTa byte-level BPE tokenizer (matches laion/clap-htsat-fused tokenizer).
// Loads vocab.json and merges.txt exported by scripts/export_clap.py.

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cassert>
#include <stdexcept>

class BPETokenizer {
public:
    static constexpr int DEFAULT_MAX_LENGTH = 77;

    // Load from the directory that contains vocab.json and merges.txt
    explicit BPETokenizer(const std::string& tokenizer_dir, int max_length = DEFAULT_MAX_LENGTH)
        : m_max_length(max_length)
    {
        load_vocab(tokenizer_dir + "/vocab.json");
        load_merges(tokenizer_dir + "/merges.txt");
        build_byte_encoder();

        // RoBERTa special token ids
        m_bos_id = token_to_id("<s>");
        m_eos_id = token_to_id("</s>");
        m_pad_id = token_to_id("<pad>");
    }

    // Tokenize a batch of strings.
    // Returns (input_ids, attention_mask), each [N * max_length] int64 row-major.
    std::pair<std::vector<int64_t>, std::vector<int64_t>>
    encode(const std::vector<std::string>& texts) const {
        int N = static_cast<int>(texts.size());
        std::size_t total = static_cast<std::size_t>(N * m_max_length);

        std::vector<int64_t> input_ids (total, static_cast<int64_t>(m_pad_id));
        std::vector<int64_t> attn_mask (total, 0LL);

        for (int i = 0; i < N; i++) {
            auto token_ids = encode_single(texts[i]);

            // BOS + content (truncated) + EOS
            std::vector<int> seq;
            seq.push_back(m_bos_id);
            int content_max = m_max_length - 2;
            for (int j = 0; j < static_cast<int>(token_ids.size()) && j < content_max; j++)
                seq.push_back(token_ids[j]);
            seq.push_back(m_eos_id);

            auto base = static_cast<std::size_t>(i * m_max_length);
            for (int j = 0; j < static_cast<int>(seq.size()); j++) {
                input_ids[base + static_cast<std::size_t>(j)] = static_cast<int64_t>(seq[j]);
                attn_mask[base + static_cast<std::size_t>(j)] = 1LL;
            }
        }

        return {std::move(input_ids), std::move(attn_mask)};
    }

private:
    // ── Byte encoder (GPT-2 / RoBERTa) ────────────────────────────────────

    void build_byte_encoder() {
        // Replicates GPT-2's bytes_to_unicode()
        // Printable bytes map to themselves; others map to codepoints 256+n
        std::vector<uint8_t> bs;
        for (int i = 33; i <= 126; i++) bs.push_back(static_cast<uint8_t>(i));
        for (int i = 161; i <= 172; i++) bs.push_back(static_cast<uint8_t>(i));
        for (int i = 174; i <= 255; i++) bs.push_back(static_cast<uint8_t>(i));

        std::unordered_set<uint8_t> bs_set(bs.begin(), bs.end());
        std::vector<int> cs(bs.begin(), bs.end());

        int n = 0;
        for (int b = 0; b < 256; b++) {
            if (bs_set.find(static_cast<uint8_t>(b)) == bs_set.end()) {
                bs.push_back(static_cast<uint8_t>(b));
                cs.push_back(256 + n++);
            }
        }

        for (size_t i = 0; i < bs.size(); i++) {
            m_byte_encoder[bs[i]] = codepoint_to_utf8(cs[i]);
        }
    }

    static std::string codepoint_to_utf8(int cp) {
        std::string s;
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return s;
    }

    // ── Vocabulary loading ─────────────────────────────────────────────────

    void load_vocab(const std::string& path) {
        // Parse flat JSON {"token": id, ...}
        std::ifstream f(path);
        if (!f.is_open()) throw std::runtime_error("Cannot open vocab.json: " + path);

        std::string line, token;
        bool in_key = false;
        std::string key;
        bool reading_val = false;
        std::string val_str;

        // Simple streaming parser — handles unicode escapes in keys
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        f.close();

        size_t i = 0;
        auto skip_ws = [&]() {
            while (i < content.size() && (content[i] == ' ' || content[i] == '\n'
                                          || content[i] == '\r' || content[i] == '\t')) i++;
        };

        // Expect '{'
        skip_ws(); assert(content[i] == '{'); i++;

        while (i < content.size()) {
            skip_ws();
            if (content[i] == '}') break;
            if (content[i] == ',') { i++; continue; }

            // Parse key string
            assert(content[i] == '"'); i++;
            key.clear();
            while (i < content.size() && content[i] != '"') {
                if (content[i] == '\\' && i + 1 < content.size()) {
                    i++;
                    if (content[i] == '"')       key += '"';
                    else if (content[i] == '\\') key += '\\';
                    else if (content[i] == 'n')  key += '\n';
                    else if (content[i] == 'r')  key += '\r';
                    else if (content[i] == 't')  key += '\t';
                    else if (content[i] == 'u' && i + 4 < content.size()) {
                        // \uXXXX unicode escape
                        int cp = std::stoi(content.substr(i + 1, 4), nullptr, 16);
                        key += codepoint_to_utf8(cp);
                        i += 4;
                    } else {
                        key += content[i];
                    }
                } else {
                    key += content[i];
                }
                i++;
            }
            assert(content[i] == '"'); i++;

            // Colon
            skip_ws(); assert(content[i] == ':'); i++;

            // Parse integer value
            skip_ws();
            val_str.clear();
            while (i < content.size() && std::isdigit(content[i])) {
                val_str += content[i++];
            }
            int id = std::stoi(val_str);

            m_vocab[key]   = id;
            m_id_to_token[id] = key;
        }
    }

    void load_merges(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) throw std::runtime_error("Cannot open merges.txt: " + path);

        std::string line;
        int rank = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            m_bpe_ranks[line] = rank++;  // "a b" → rank
        }
    }

    // ── BPE ────────────────────────────────────────────────────────────────

    // Split a UTF-8 string into individual unicode character strings
    static std::vector<std::string> split_utf8(const std::string& s) {
        std::vector<std::string> chars;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            chars.push_back(s.substr(i, len));
            i += len;
        }
        return chars;
    }

    std::vector<std::string> bpe(std::vector<std::string> symbols) const {
        while (symbols.size() > 1) {
            int best_rank = -1, best_idx = -1;
            for (int i = 0; i < static_cast<int>(symbols.size()) - 1; i++) {
                auto it = m_bpe_ranks.find(symbols[i] + " " + symbols[i + 1]);
                if (it != m_bpe_ranks.end()) {
                    if (best_rank < 0 || it->second < best_rank) {
                        best_rank = it->second;
                        best_idx  = i;
                    }
                }
            }
            if (best_idx < 0) break;

            std::vector<std::string> merged;
            merged.reserve(symbols.size() - 1);
            for (int i = 0; i < static_cast<int>(symbols.size()); i++) {
                if (i == best_idx) {
                    merged.push_back(symbols[i] + symbols[i + 1]);
                    i++;  // skip next
                } else {
                    merged.push_back(symbols[i]);
                }
            }
            symbols = std::move(merged);
        }
        return symbols;
    }

    // Encode a single word (already byte-encoded) to token IDs
    std::vector<int> encode_word(const std::string& word_bytes) const {
        auto symbols = split_utf8(word_bytes);
        auto merged  = bpe(symbols);

        std::vector<int> ids;
        for (const auto& tok : merged) {
            auto it = m_vocab.find(tok);
            if (it != m_vocab.end()) ids.push_back(it->second);
            // Unknown sub-words are silently skipped (rare for well-formed text)
        }
        return ids;
    }

    // Encode a full string (lowercase, split on spaces)
    std::vector<int> encode_single(const std::string& text) const {
        // RoBERTa lowercases and uses byte-level BPE with Ġ prefix for non-first words
        std::string lower;
        for (unsigned char c : text) lower += static_cast<char>(std::tolower(c));

        std::istringstream iss(lower);
        std::string word;
        std::vector<int> all_ids;
        bool first = true;

        while (iss >> word) {
            // Build byte-level representation
            // Non-first words get a space byte (32 → "Ġ") prepended
            std::string byte_str;
            if (!first) {
                byte_str += m_byte_encoder.at(static_cast<uint8_t>(32));  // "Ġ"
            }
            first = false;

            for (unsigned char c : word) {
                byte_str += m_byte_encoder.at(c);
            }

            auto ids = encode_word(byte_str);
            all_ids.insert(all_ids.end(), ids.begin(), ids.end());
        }

        return all_ids;
    }

    int token_to_id(const std::string& tok) const {
        auto it = m_vocab.find(tok);
        if (it == m_vocab.end()) throw std::runtime_error("Special token not found: " + tok);
        return it->second;
    }

    // ── Members ────────────────────────────────────────────────────────────
    std::unordered_map<std::string, int>  m_vocab;
    std::unordered_map<int, std::string>  m_id_to_token;
    std::unordered_map<std::string, int>  m_bpe_ranks;   // "a b" → rank
    std::unordered_map<uint8_t, std::string> m_byte_encoder;

    int m_max_length;
    int m_bos_id, m_eos_id, m_pad_id;
};

#endif //CLAP_TILDE_BPE_TOKENIZER_H

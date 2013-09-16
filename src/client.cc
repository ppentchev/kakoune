#include "client.hh"

#include "color_registry.hh"
#include "context.hh"
#include "editor.hh"
#include "event_manager.hh"
#include "normal.hh"
#include "register_manager.hh"
#include "buffer_manager.hh"
#include "user_interface.hh"
#include "utf8.hh"
#include "window.hh"
#include "file.hh"

#include <unordered_map>

namespace Kakoune
{

class InputMode
{
public:
    InputMode(Client& client) : m_client(client) {}
    virtual ~InputMode() {}
    InputMode(const InputMode&) = delete;
    InputMode& operator=(const InputMode&) = delete;

    virtual void on_key(Key key) = 0;
    Context& context() const { return m_client.context(); }

    using Insertion = Client::Insertion;
    Insertion& last_insert() { return m_client.m_last_insert; }

protected:
    InputMode& reset_normal_mode();
private:
    Client& m_client;
};

namespace InputModes
{

static constexpr std::chrono::milliseconds idle_timeout{100};

class Normal : public InputMode
{
public:
    Normal(Client& client)
        : InputMode(client),
          m_idle_timer{Clock::now() + idle_timeout, [this](Timer& timer) {
              context().hooks().run_hook("NormalIdle", "", context());
          }}
    {
        context().hooks().run_hook("NormalBegin", "", context());
    }

    ~Normal()
    {
        context().hooks().run_hook("NormalEnd", "", context());
    }

    void on_key(Key key) override
    {
        if (key.modifiers == Key::Modifiers::None and isdigit(key.key))
            m_count = m_count * 10 + key.key - '0';
        else
        {
            auto it = keymap.find(key);
            if (it != keymap.end())
            {
                context().numeric_param() = m_count;
                it->second(context());
            }
            m_count = 0;
        }
        context().hooks().run_hook("NormalKey", key_to_str(key), context());
        m_idle_timer.set_next_date(Clock::now() + idle_timeout);
    }

private:
    int m_count = 0;
    Timer m_idle_timer;
};

class LineEditor
{
public:
    void handle_key(Key key)
    {
        if (key == Key::Left or
            key == Key{Key::Modifiers::Control, 'b'})
        {
            if (m_cursor_pos > 0)
                --m_cursor_pos;
        }
        else if (key == Key::Right or
                 key == Key{Key::Modifiers::Control, 'f'})
        {
            if (m_cursor_pos < m_line.char_length())
                ++m_cursor_pos;
        }
        else if (key == Key::Home)
            m_cursor_pos = 0;
        else if (key == Key::End)
            m_cursor_pos = m_line.char_length();
        else if (key == Key::Backspace)
        {
            if (m_cursor_pos != 0)
            {
                m_line = m_line.substr(0, m_cursor_pos - 1)
                       + m_line.substr(m_cursor_pos);

                --m_cursor_pos;
            }
        }
        else
        {
            m_line = m_line.substr(0, m_cursor_pos) + codepoint_to_str(key.key)
                   + m_line.substr(m_cursor_pos);
            ++m_cursor_pos;
        }
    }

    void insert(const String& str)
    {
        insert_from(m_cursor_pos, str);
    }

    void insert_from(CharCount start, const String& str)
    {
        kak_assert(start <= m_cursor_pos);
        m_line = m_line.substr(0, start) + str
               + m_line.substr(m_cursor_pos);
       m_cursor_pos = start + str.char_length();
    }

    void reset(String line)
    {
        m_line = std::move(line);
        m_cursor_pos = m_line.char_length();
    }

    const String& line() const { return m_line; }
    CharCount cursor_pos() const { return m_cursor_pos; }

    DisplayLine build_display_line() const
    {
        kak_assert(m_cursor_pos <= m_line.char_length());
        if (m_cursor_pos == m_line.char_length())
            return DisplayLine{{ {m_line, get_color("StatusLine")},
                                 {" "_str, get_color("StatusCursor")} }};
        else
            return DisplayLine({ { m_line.substr(0, m_cursor_pos), get_color("StatusLine") },
                                 { m_line.substr(m_cursor_pos, 1), get_color("StatusCursor") },
                                 { m_line.substr(m_cursor_pos+1), get_color("StatusLine") } });
    }
private:
    CharCount      m_cursor_pos = 0;
    String         m_line;
};

class Menu : public InputMode
{
public:
    Menu(Client& client, memoryview<String> choices,
         MenuCallback callback)
        : InputMode(client),
          m_callback(callback), m_choices(choices.begin(), choices.end()),
          m_selected(m_choices.begin())
    {
        DisplayCoord menu_pos{ context().ui().dimensions().line, 0_char };
        context().ui().menu_show(choices, menu_pos, get_color("MenuForeground"),
                                 get_color("MenuBackground"), MenuStyle::Prompt);
    }

    void on_key(Key key) override
    {
        auto match_filter = [this](const String& str) {
            return boost::regex_match(str.begin(), str.end(), m_filter);
        };

        if (key == Key(Key::Modifiers::Control, 'm'))
        {
            context().ui().menu_hide();
            context().print_status(DisplayLine{});
            reset_normal_mode();
            int selected = m_selected - m_choices.begin();
            m_callback(selected, MenuEvent::Validate, context());
            return;
        }
        else if (key == Key::Escape or key == Key{ Key::Modifiers::Control, 'c' })
        {
            if (m_edit_filter)
            {
                m_edit_filter = false;
                m_filter = boost::regex(".*");
                m_filter_editor.reset("");
                context().print_status(DisplayLine{});
            }
            else
            {
                context().ui().menu_hide();
                reset_normal_mode();
                int selected = m_selected - m_choices.begin();
                m_callback(selected, MenuEvent::Abort, context());
            }
        }
        else if (key == Key::Down or
                 key == Key(Key::Modifiers::Control, 'i') or
                 key == Key(Key::Modifiers::Control, 'n') or
                 key == Key(Key::Modifiers::None, 'j'))
        {
            auto it = std::find_if(m_selected+1, m_choices.end(), match_filter);
            if (it == m_choices.end())
                it = std::find_if(m_choices.begin(), m_selected, match_filter);
            select(it);
        }
        else if (key == Key::Up or
                 key == Key::BackTab or
                 key == Key(Key::Modifiers::Control, 'p') or
                 key == Key(Key::Modifiers::None, 'k'))
        {
            ChoiceList::const_reverse_iterator selected(m_selected+1);
            auto it = std::find_if(selected+1, m_choices.rend(), match_filter);
            if (it == m_choices.rend())
                it = std::find_if(m_choices.rbegin(), selected, match_filter);
            select(it.base()-1);
        }
        else if (key == '/' and not m_edit_filter)
        {
            m_edit_filter = true;
        }
        else if (m_edit_filter)
        {
            m_filter_editor.handle_key(key);

            auto search = ".*" + m_filter_editor.line() + ".*";
            m_filter = boost::regex(search.begin(), search.end());
            auto it = std::find_if(m_selected, m_choices.end(), match_filter);
            if (it == m_choices.end())
                it = std::find_if(m_choices.begin(), m_selected, match_filter);
            select(it);
        }

        if (m_edit_filter)
        {
            auto display_line = m_filter_editor.build_display_line();
            display_line.insert(display_line.begin(), { "filter:"_str, get_color("Prompt") });
            context().print_status(display_line);
        }
   }

private:
    MenuCallback m_callback;

    using ChoiceList = std::vector<String>;
    const ChoiceList m_choices;
    ChoiceList::const_iterator m_selected;

    void select(ChoiceList::const_iterator it)
    {
        m_selected = it;
        int selected = m_selected - m_choices.begin();
        context().ui().menu_select(selected);
        m_callback(selected, MenuEvent::Select, context());
    }

    boost::regex m_filter = boost::regex(".*");
    bool         m_edit_filter = false;
    LineEditor   m_filter_editor;
};

String common_prefix(memoryview<String> strings)
{
    String res;
    if (strings.empty())
        return res;
    res = strings[0];
    for (auto& str : strings)
    {
        ByteCount len = std::min(res.length(), str.length());
        ByteCount common_len = 0;
        while (common_len < len and str[common_len] == res[common_len])
            ++common_len;
        if (common_len != res.length())
            res = res.substr(0, common_len);
    }
    return res;
}

class Prompt : public InputMode
{
public:
    Prompt(Client& client, const String& prompt,
           ColorPair colors, Completer completer, PromptCallback callback)
        : InputMode(client), m_prompt(prompt), m_prompt_colors(colors),
          m_completer(completer), m_callback(callback)
    {
        m_history_it = ms_history[m_prompt].end();
        display();
    }

    void on_key(Key key) override
    {
        std::vector<String>& history = ms_history[m_prompt];
        const String& line = m_line_editor.line();

        if (m_insert_reg)
        {
            String reg = RegisterManager::instance()[key.key].values(context())[0];
            m_line_editor.insert(reg);
            m_insert_reg = false;
        }
        else if (key == Key{Key::Modifiers::Control, 'm'}) // enter
        {
            if (not line.empty())
            {
                std::vector<String>::iterator it;
                while ((it = find(history, line)) != history.end())
                    history.erase(it);
                history.push_back(line);
            }
            context().print_status(DisplayLine{});
            context().ui().menu_hide();
            reset_normal_mode();
            // call callback after reset_normal_mode so that callback
            // may change the mode
            m_callback(line, PromptEvent::Validate, context());
            return;
        }
        else if (key == Key::Escape or key == Key { Key::Modifiers::Control, 'c' })
        {
            context().print_status(DisplayLine{});
            context().ui().menu_hide();
            reset_normal_mode();
            m_callback(line, PromptEvent::Abort, context());
            return;
        }
        else if (key == Key{Key::Modifiers::Control, 'r'})
        {
            m_insert_reg = true;
        }
        else if (key == Key::Up or
                 key == Key{Key::Modifiers::Control, 'p'})
        {
            if (m_history_it != history.begin())
            {
                if (m_history_it == history.end())
                   m_prefix = line;
                auto it = m_history_it;
                // search for the previous history entry matching typed prefix
                ByteCount prefix_length = m_prefix.length();
                do
                {
                    --it;
                    if (it->substr(0, prefix_length) == m_prefix)
                    {
                        m_history_it = it;
                        m_line_editor.reset(*it);
                        break;
                    }
                } while (it != history.begin());
            }
        }
        else if (key == Key::Down or // next
                 key == Key{Key::Modifiers::Control, 'n'})
        {
            if (m_history_it != history.end())
            {
                ByteCount prefix_length = m_prefix.length();
                // search for the next history entry matching typed prefix
                ++m_history_it;
                while (m_history_it != history.end() and
                       m_history_it->substr(0, prefix_length) != m_prefix)
                    ++m_history_it;

                if (m_history_it != history.end())
                    m_line_editor.reset(*m_history_it);
                else
                    m_line_editor.reset(m_prefix);
            }
        }
        else if (key == Key(Key::Modifiers::Control, 'i') or // tab completion
                 key == Key::BackTab)
        {
            const bool reverse = (key == Key::BackTab);
            CandidateList& candidates = m_completions.candidates;
            // first try, we need to ask our completer for completions
            if (m_current_completion == -1)
            {
                m_completions = m_completer(context(), line,
                                            line.byte_count_to(m_line_editor.cursor_pos()));
                if (candidates.empty())
                    return;

                context().ui().menu_hide();
                DisplayCoord menu_pos{ context().ui().dimensions().line, 0_char };
                context().ui().menu_show(candidates, menu_pos, get_color("MenuForeground"),
                                         get_color("MenuBackground"), MenuStyle::Prompt);

                bool use_common_prefix = context().options()["complete_prefix"].get<bool>();
                String prefix = use_common_prefix ? common_prefix(candidates) : String();
                if (m_completions.end - m_completions.start > prefix.length())
                    prefix = line.substr(m_completions.start,
                                         m_completions.end - m_completions.start);

                auto it = find(candidates, prefix);
                if (it == candidates.end())
                {
                    m_current_completion = use_common_prefix ? candidates.size() : 0;
                    candidates.push_back(std::move(prefix));
                }
                else
                    m_current_completion = use_common_prefix ? it - candidates.begin() : 0;
            }
            else if (not reverse and ++m_current_completion >= candidates.size())
                m_current_completion = 0;
            else if (reverse and --m_current_completion < 0)
                m_current_completion = candidates.size()-1;

            const String& completion = candidates[m_current_completion];
            context().ui().menu_select(m_current_completion);

            m_line_editor.insert_from(line.char_count_to(m_completions.start),
                                      completion);

            // when we have only one completion candidate, make next tab complete
            // from the new content.
            if (candidates.size() == 1)
                m_current_completion = -1;
        }
        else
        {
            context().ui().menu_hide();
            m_current_completion = -1;
            m_line_editor.handle_key(key);
        }
        display();
        m_callback(line, PromptEvent::Change, context());
    }

    void set_prompt_colors(ColorPair colors)
    {
        if (colors != m_prompt_colors)
        {
            m_prompt_colors = colors;
            display();
        }
    }

private:
    void display() const
    {
        auto display_line = m_line_editor.build_display_line();
        display_line.insert(display_line.begin(), { m_prompt, m_prompt_colors });
        context().print_status(display_line);
    }

    PromptCallback m_callback;
    Completer      m_completer;
    const String   m_prompt;
    ColorPair      m_prompt_colors;
    Completions    m_completions;
    int            m_current_completion = -1;
    String         m_prefix;
    LineEditor     m_line_editor;
    bool           m_insert_reg = false;

    static std::unordered_map<String, std::vector<String>> ms_history;
    std::vector<String>::iterator m_history_it;
};
std::unordered_map<String, std::vector<String>> Prompt::ms_history;

class NextKey : public InputMode
{
public:
    NextKey(Client& client, KeyCallback callback)
        : InputMode(client), m_callback(callback) {}

    void on_key(Key key) override
    {
        reset_normal_mode();
        m_callback(key, context());
   }

private:
    KeyCallback m_callback;
};

struct BufferCompletion
{
    BufferCoord begin;
    BufferCoord end;
    CandidateList  candidates;
    size_t         timestamp;

    bool is_valid() const { return not candidates.empty(); }
};


class BufferCompleter : public OptionManagerWatcher_AutoRegister
{
public:
    BufferCompleter(const Context& context)
        : OptionManagerWatcher_AutoRegister(context.options()), m_context(context)
    {}
    BufferCompleter(const BufferCompleter&) = delete;
    BufferCompleter& operator=(const BufferCompleter&) = delete;

    void select(int offset)
    {
        if (not setup_ifn())
            return;

        auto& buffer = m_context.buffer();
        m_current_candidate = (m_current_candidate + offset) % (int)m_matching_candidates.size();
        if (m_current_candidate < 0)
            m_current_candidate += m_matching_candidates.size();
        const String& candidate = m_matching_candidates[m_current_candidate];
        const auto& cursor_pos = m_context.editor().main_selection().last();
        const auto prefix_len = buffer.distance(m_completions.begin, cursor_pos);
        const auto suffix_len = buffer.distance(cursor_pos, m_completions.end);
        const auto buffer_len = buffer.byte_count();

        auto ref = buffer.string(m_completions.begin, m_completions.end);
        for (auto& sel : m_context.editor().selections())
        {
            auto offset = buffer.offset(sel.last());
            auto pos = buffer.iterator_at(sel.last());
            if (offset >= prefix_len and offset + suffix_len < buffer_len and
                std::equal(ref.begin(), ref.end(), pos - prefix_len))
            {
                pos = buffer.erase(pos - prefix_len, pos + suffix_len);
                buffer.insert(pos, candidate);
            }
        }
        m_completions.end   = cursor_pos;
        m_completions.begin = buffer.advance(m_completions.end, -candidate.length());
        m_completions.timestamp = m_context.buffer().timestamp();
        m_context.ui().menu_select(m_current_candidate);

        // when we select a match, remove non displayed matches from the candidates
        // which are considered as invalid with the new completion timestamp
        m_completions.candidates.clear();
        std::copy(m_matching_candidates.begin(), m_matching_candidates.end()-1,
                  std::back_inserter(m_completions.candidates));
    }

    void update()
    {
        if (m_completions.is_valid())
        {
            ByteCount longest_completion = 0;
            for (auto& candidate : m_completions.candidates)
                 longest_completion = std::max(longest_completion, candidate.length());

            BufferCoord cursor = m_context.editor().main_selection().last();
            BufferCoord compl_beg = m_completions.begin;
            if (cursor.line == compl_beg.line and
                is_in_range(cursor.column - compl_beg.column,
                            ByteCount{0}, longest_completion-1))
            {
                String prefix = m_context.buffer().string(compl_beg, cursor);

                if (m_context.buffer().timestamp() == m_completions.timestamp)
                    m_matching_candidates = m_completions.candidates;
                else
                {
                    m_matching_candidates.clear();
                    for (auto& candidate : m_completions.candidates)
                    {
                        if (candidate.substr(0, prefix.length()) == prefix)
                            m_matching_candidates.push_back(candidate);
                    }
                }
                if (not m_matching_candidates.empty())
                {
                    m_context.ui().menu_hide();
                    m_current_candidate = m_matching_candidates.size();
                    m_completions.end = cursor;
                    menu_show();
                    m_matching_candidates.push_back(prefix);
                    return;
                }
            }
        }
        reset();
        setup_ifn();
    }

    void reset()
    {
        m_completions = BufferCompletion{};
        m_context.ui().menu_hide();
    }

    template<BufferCompletion (BufferCompleter::*complete_func)(const Buffer&, BufferCoord)>
    bool try_complete()
    {
        auto& buffer = m_context.buffer();
        BufferCoord cursor_pos = m_context.editor().main_selection().last();
        m_completions = (this->*complete_func)(buffer, cursor_pos);
        if (not m_completions.is_valid())
            return false;

        kak_assert(cursor_pos >= m_completions.begin);
        m_matching_candidates = m_completions.candidates;
        m_current_candidate = m_matching_candidates.size();
        m_context.ui().menu_hide();
        menu_show();
        m_matching_candidates.push_back(buffer.string(m_completions.begin, m_completions.end));
        return true;
    }
    using StringList = std::vector<String>;

    template<bool other_buffers>
    BufferCompletion complete_word(const Buffer& buffer, BufferCoord cursor_pos)
    {
       auto pos = buffer.iterator_at(cursor_pos);
       if (pos == buffer.begin() or not is_word(*utf8::previous(pos)))
           return {};

        auto end = buffer.iterator_at(cursor_pos);
        auto begin = end-1;
        while (begin != buffer.begin() and is_word(*begin))
            --begin;
        if (not is_word(*begin))
            ++begin;

        String ex = R"(\<\Q)" + String{begin, end} + R"(\E\w+\>)";
        Regex re(ex.begin(), ex.end());
        using RegexIt = boost::regex_iterator<BufferIterator>;

        std::unordered_set<String> matches;
        for (RegexIt it(buffer.begin(), buffer.end(), re), re_end; it != re_end; ++it)
        {
            auto& match = (*it)[0];
            if (match.first <= pos and pos < match.second)
                continue;
            matches.insert(String{match.first, match.second});
        }
        if (other_buffers)
        {
            for (const auto& buf : BufferManager::instance())
            {
                if (buf.get() == &buffer)
                    continue;
                for (RegexIt it(buf->begin(), buf->end(), re), re_end; it != re_end; ++it)
                {
                    auto& match = (*it)[0];
                    matches.insert(String{match.first, match.second});
                }
            }
        }
        CandidateList result;
        std::copy(make_move_iterator(matches.begin()),
                  make_move_iterator(matches.end()),
                  inserter(result, result.begin()));
        std::sort(result.begin(), result.end());
        return { begin.coord(), end.coord(), std::move(result), buffer.timestamp() };
    }

    BufferCompletion complete_filename(const Buffer& buffer, BufferCoord cursor_pos)
    {
        auto pos = buffer.iterator_at(cursor_pos);
        auto begin = pos;

        auto is_filename = [](char c)
        {
            return isalnum(c) or c == '/' or c == '.' or c == '_' or c == '-';
        };
        while (begin != buffer.begin() and is_filename(*(begin-1)))
            --begin;

        if (begin == pos)
            return {};

        String prefix{begin, pos};
        StringList res;
        if (prefix.front() == '/')
            res = Kakoune::complete_filename(prefix, Regex{});
        else
        {
            for (auto dir : options()["path"].get<StringList>())
            {
                if (not dir.empty() and dir.back() != '/')
                    dir += '/';
                for (auto& filename : Kakoune::complete_filename(dir + prefix, Regex{}))
                    res.push_back(filename.substr(dir.length()));
            }
        }
        if (res.empty())
            return {};
        return { begin.coord(), pos.coord(), std::move(res), buffer.timestamp() };
    }

    BufferCompletion complete_option(const Buffer& buffer, BufferCoord cursor_pos)
    {
        const StringList& opt = options()["completions"].get<StringList>();
        if (opt.empty())
            return {};

        auto& desc = opt[0];
        static const Regex re(R"((\d+)\.(\d+)(?:\+(\d+))?@(\d+))");
        boost::smatch match;
        if (boost::regex_match(desc.begin(), desc.end(), match, re))
        {
            BufferCoord coord{ str_to_int(match[1].str()) - 1, str_to_int(match[2].str()) - 1 };
            if (not buffer.is_valid(coord))
                return {};
            auto end = coord;
            if (match[3].matched)
            {
                ByteCount len = str_to_int(match[3].str());
                end = buffer.advance(coord, len);
            }
            size_t timestamp = (size_t)str_to_int(match[4].str());

            ByteCount longest_completion = 0;
            for (auto it = opt.begin() + 1; it != opt.end(); ++it)
                 longest_completion = std::max(longest_completion, it->length());

            if (timestamp == buffer.timestamp() and
                cursor_pos.line == coord.line and cursor_pos.column <= coord.column and
                buffer.distance(coord, cursor_pos) < longest_completion)
                return { coord, end, { opt.begin() + 1, opt.end() }, timestamp };
        }
        return {};
    }

    BufferCompletion complete_line(const Buffer& buffer, BufferCoord cursor_pos)
    {
        String prefix = buffer[cursor_pos.line].substr(0_byte, cursor_pos.column);
        StringList res;
        for (LineCount l = 0_line; l < buffer.line_count(); ++l)
        {
            if (l == cursor_pos.line)
                continue;
            ByteCount len = buffer[l].length();
            if (len > cursor_pos.column and std::equal(prefix.begin(), prefix.end(), buffer[l].begin()))
                res.push_back(buffer[l].substr(0_byte, len-1));
        }
        if (res.empty())
            return {};
        std::sort(res.begin(), res.end());
        res.erase(std::unique(res.begin(), res.end()), res.end());
        return { cursor_pos.line, cursor_pos, std::move(res), buffer.timestamp() };
    }

private:
    void on_option_changed(const Option& opt) override
    {
        if (opt.name() == "completions")
        {
            reset();
            setup_ifn();
        }
    }

    void menu_show()
    {
        DisplayCoord menu_pos = m_context.window().display_position(m_completions.begin);
        m_context.ui().menu_show(m_matching_candidates, menu_pos,
                                 get_color("MenuForeground"),
                                 get_color("MenuBackground"),
                                 MenuStyle::Inline);
        m_context.ui().menu_select(m_current_candidate);
    }

    bool setup_ifn()
    {
        if (not m_completions.is_valid())
        {
            auto& completers = options()["completers"].get<StringList>();
            if (contains(completers, "option") and try_complete<&BufferCompleter::complete_option>())
                return true;
            if (contains(completers, "word=buffer") and try_complete<&BufferCompleter::complete_word<false>>())
                return true;
            if (contains(completers, "word=all") and try_complete<&BufferCompleter::complete_word<true>>())
                return true;
            if (contains(completers, "filename") and try_complete<&BufferCompleter::complete_filename>())
                return true;

            return false;
        }
        return true;
    }

    const Context&   m_context;
    BufferCompletion m_completions;
    CandidateList    m_matching_candidates;
    int              m_current_candidate = -1;
};

class Insert : public InputMode
{
public:
    Insert(Client& client, InsertMode mode)
        : InputMode(client),
          m_inserter(context().editor(), mode),
          m_completer(context()),
          m_idle_timer{Clock::now() + idle_timeout,
                       [this](Timer& timer) {
                           context().hooks().run_hook("InsertIdle", "", context());
                           m_completer.update();
                       }}
    {
        last_insert().first = mode;
        last_insert().second.clear();
        context().hooks().run_hook("InsertBegin", "", context());
    }

    void on_key(Key key) override
    {
        if (&context().editor() != &m_inserter.editor())
            return reset_normal_mode().on_key(key);

        last_insert().second.push_back(key);
        if (m_mode == Mode::InsertReg)
        {
            if (key.modifiers == Key::Modifiers::None)
                m_inserter.insert(RegisterManager::instance()[key.key].values(context()));
            m_mode = Mode::Default;
            return;
        }
        if (m_mode == Mode::Complete)
        {
            if (key.key == 'f')
                m_completer.try_complete<&BufferCompleter::complete_filename>();
            if (key.key == 'w')
                m_completer.try_complete<&BufferCompleter::complete_word<true>>();
            if (key.key == 'o')
                m_completer.try_complete<&BufferCompleter::complete_option>();
            if (key.key == 'l')
                m_completer.try_complete<&BufferCompleter::complete_line>();
            m_mode = Mode::Default;
            return;
        }

        bool update_completions = true;
        bool moved = false;
        if (key == Key::Escape or key == Key{ Key::Modifiers::Control, 'c' })
        {
            context().hooks().run_hook("InsertEnd", "", context());
            m_completer.reset();
            reset_normal_mode();
        }
        else if (key == Key::Backspace)
            m_inserter.erase();
        else if (key == Key::Left)
        {
            m_inserter.move_cursors(-1_char);
            moved = true;
        }
        else if (key == Key::Right)
        {
            m_inserter.move_cursors(1_char);
            moved = true;
        }
        else if (key == Key::Up)
        {
            m_inserter.move_cursors(-1_line);
            moved = true;
        }
        else if (key == Key::Down)
        {
            m_inserter.move_cursors(1_line);
            moved = true;
        }
        else if (key.modifiers == Key::Modifiers::None)
        {
            m_inserter.insert(codepoint_to_str(key.key));
            context().hooks().run_hook("InsertKey", key_to_str(key), context());
        }
        else if (key == Key{ Key::Modifiers::Control, 'r' })
            m_mode = Mode::InsertReg;
        else if ( key == Key{ Key::Modifiers::Control, 'm' })
            m_inserter.insert(String() + '\n');
        else if ( key == Key{ Key::Modifiers::Control, 'i' })
            m_inserter.insert(String() + '\t');
        else if ( key == Key{ Key::Modifiers::Control, 'n' })
        {
            m_completer.select(1);
            update_completions = false;
        }
        else if ( key == Key{ Key::Modifiers::Control, 'p' })
        {
            m_completer.select(-1);
            update_completions = false;
        }
        else if ( key == Key{ Key::Modifiers::Control, 'x' })
            m_mode = Mode::Complete;

        if (update_completions)
            m_idle_timer.set_next_date(Clock::now() + idle_timeout);
        if (moved)
            context().hooks().run_hook("InsertMove", key_to_str(key), context());
    }
private:
    enum class Mode { Default, Complete, InsertReg };
    Mode m_mode = Mode::Default;
    IncrementalInserter m_inserter;
    BufferCompleter     m_completer;
    Timer               m_idle_timer;
};

}

InputMode& InputMode::reset_normal_mode()
{
    m_client.m_mode_trash.emplace_back(std::move(m_client.m_mode));
    m_client.m_mode.reset(new InputModes::Normal(m_client));
    return *m_client.m_mode;
}


Client::Client(std::unique_ptr<UserInterface>&& ui, Editor& editor, String name)
    : m_ui(std::move(ui)), m_context(*this, editor), m_mode(new InputModes::Normal(*this)), m_name(name)
{
}

Client::~Client()
{
}

void Client::insert(InsertMode mode)
{
    m_mode_trash.emplace_back(std::move(m_mode));
    m_mode.reset(new InputModes::Insert(*this, mode));
}

void Client::repeat_last_insert()
{
    if (m_last_insert.second.empty())
        return;

    std::vector<Key> keys;
    swap(keys, m_last_insert.second);
    // context.last_insert will be refilled by the new Insert
    // this is very inefficient.
    m_mode_trash.emplace_back(std::move(m_mode));
    m_mode.reset(new InputModes::Insert(*this, m_last_insert.first));
    for (auto& key : keys)
        m_mode->on_key(key);
    kak_assert(dynamic_cast<InputModes::Normal*>(m_mode.get()) != nullptr);
}

void Client::prompt(const String& prompt, ColorPair prompt_colors,
                          Completer completer, PromptCallback callback)
{
    m_mode_trash.emplace_back(std::move(m_mode));
    m_mode.reset(new InputModes::Prompt(*this, prompt, prompt_colors,
                                        completer, callback));
}

void Client::set_prompt_colors(ColorPair prompt_colors)
{
    InputModes::Prompt* prompt = dynamic_cast<InputModes::Prompt*>(m_mode.get());
    if (prompt)
        prompt->set_prompt_colors(prompt_colors);
}

void Client::menu(memoryview<String> choices,
                        MenuCallback callback)
{
    m_mode_trash.emplace_back(std::move(m_mode));
    m_mode.reset(new InputModes::Menu(*this, choices, callback));
}

void Client::on_next_key(KeyCallback callback)
{
    m_mode_trash.emplace_back(std::move(m_mode));
    m_mode.reset(new InputModes::NextKey(*this, callback));
}

bool is_valid(Key key)
{
    return key != Key::Invalid and key.key <= 0x10FFFF;
}

void Client::handle_available_input()
{
    while (m_ui->is_key_available())
    {
        handle_key(m_ui->get_key());
        m_mode_trash.clear();
    }
    m_context.window().forget_timestamp();
}

void Client::handle_key(Key key)
{
    if (is_valid(key))
    {
        const bool was_recording = is_recording();

        m_mode->on_key(key);

        // do not record the key that made us enter or leave recording mode.
        if (was_recording and is_recording())
            m_recorded_keys += key_to_str(key);
    }
}

void Client::start_recording(char reg)
{
    kak_assert(m_recording_reg == 0);
    m_recorded_keys = "";
    m_recording_reg = reg;
}

bool Client::is_recording() const
{
    return m_recording_reg != 0;
}

void Client::stop_recording()
{
    kak_assert(m_recording_reg != 0);
    RegisterManager::instance()[m_recording_reg] = memoryview<String>(m_recorded_keys);
    m_recording_reg = 0;
}

void Client::print_status(DisplayLine status_line)
{
    m_status_line = std::move(status_line);
    m_context.window().forget_timestamp();
}

static DisplayLine generate_mode_line(Client& client)
{
    auto& context = client.context();
    auto pos = context.editor().main_selection().last();
    auto col = context.buffer()[pos.line].char_count_to(pos.column);

    std::ostringstream oss;
    oss << context.buffer().display_name()
        << " " << (int)pos.line+1 << ":" << (int)col+1;
    if (context.buffer().is_modified())
        oss << " [+]";
    if (context.client().is_recording())
       oss << " [recording]";
    if (context.buffer().flags() & Buffer::Flags::New)
        oss << " [new file]";
    oss << " [" << context.editor().selections().size() << " sel]";
    if (context.editor().is_editing())
        oss << " [insert]";
    oss << " - " << client.name();
    return { oss.str(), get_color("StatusLine") };
}

void Client::redraw_ifn()
{
    if (m_context.window().timestamp() != m_context.buffer().timestamp())
    {
        DisplayCoord dimensions = m_context.ui().dimensions();
        if (dimensions == DisplayCoord{0,0})
            return;
        m_context.window().set_dimensions(dimensions);
        m_context.window().update_display_buffer();;

        m_context.ui().draw(m_context.window().display_buffer(),
                            m_status_line, generate_mode_line(*this));
    }
}

}
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QKeySequence>
#include <QMainWindow>
#include <QMessageBox>
#include <QSaveFile>
#include <QShortcut>
#include <QStatusBar>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

#include <Qsci/qscilexerlua.h>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciapis.h>

namespace {

QString default_script() {
    return
        "-- Script Lua para o emulador SNES.\n"
        "-- Salve com Ctrl-S. O emulador recarrega quando detectar mudanca.\n"
        "\n"
        "function on_frame(frame)\n"
        "    snes.clear_input()\n"
        "\n"
        "end\n";
}

QString read_script(const QString &path) {
    QFile file(path);
    if (!file.exists()) {
        QFileInfo info(path);
        QDir().mkpath(info.absolutePath());
        QSaveFile created(path);
        if (created.open(QIODevice::WriteOnly | QIODevice::Text)) {
            created.write(default_script().toUtf8());
            created.commit();
        }
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return default_script();
    }
    return QString::fromUtf8(file.readAll());
}

class LuaEditorWindow : public QMainWindow {
public:
    explicit LuaEditorWindow(QString path) : path_(std::move(path)) {
        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        layout->setContentsMargins(0, 0, 0, 0);

        editor_ = new QsciScintilla(central);
        lexer_ = new QsciLexerLua(editor_);
        apis_ = new QsciAPIs(lexer_);
        configure_lexer();
        configure_editor();
        configure_api();

        editor_->setText(read_script(path_));
        add_document_words(editor_->text());
        apis_->prepare();
        layout->addWidget(editor_);
        setCentralWidget(central);

        setWindowTitle("SNES Lua Studio - " + path_);
        resize(1180, 820);
        statusBar()->showMessage("Ctrl-S salva | Ctrl-Space autocomplete | Ctrl-R salva");

        new QShortcut(QKeySequence::Save, this, [this]() { save(); });
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_R), this,
                      [this]() { save(); });
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space), this,
                      [this]() { editor_->autoCompleteFromAll(); });
    }

private:
    void configure_editor() {
        editor_->setLexer(lexer_);
        editor_->setUtf8(true);
        editor_->setEolMode(QsciScintilla::EolUnix);
        editor_->setAutoIndent(true);
        editor_->setIndentationsUseTabs(false);
        editor_->setIndentationWidth(4);
        editor_->setTabWidth(4);
        editor_->setBackspaceUnindents(true);
        editor_->setIndentationGuides(true);
        editor_->setBraceMatching(QsciScintilla::SloppyBraceMatch);
        editor_->setCaretLineVisible(true);
        editor_->setCaretWidth(2);
        editor_->setMarginsForegroundColor(QColor("#7d8590"));
        editor_->setMarginsBackgroundColor(QColor("#161b22"));
        editor_->setMarginType(0, QsciScintilla::NumberMargin);
        editor_->setMarginWidth(0, "00000");
        editor_->setFolding(QsciScintilla::BoxedTreeFoldStyle);
        editor_->setAutoCompletionSource(QsciScintilla::AcsAll);
        editor_->setAutoCompletionThreshold(1);
        editor_->setAutoCompletionCaseSensitivity(false);
        editor_->setAutoCompletionReplaceWord(true);
        editor_->setAutoCompletionShowSingle(true);
        editor_->setAutoCompletionUseSingle(QsciScintilla::AcusExplicit);
        editor_->setAutoCompletionFillupsEnabled(true);
        editor_->setAutoCompletionFillups("().,:");
        editor_->setAutoCompletionWordSeparators(QStringList());
        editor_->SendScintilla(QsciScintilla::SCI_SETWORDCHARS,
                               "abcdefghijklmnopqrstuvwxyz"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "0123456789_.:");
        editor_->SendScintilla(QsciScintilla::SCI_AUTOCSETIGNORECASE, 1);
        editor_->SendScintilla(QsciScintilla::SCI_AUTOCSETMAXHEIGHT, 18);
        editor_->SendScintilla(QsciScintilla::SCI_AUTOCSETMAXWIDTH, 80);
        editor_->SendScintilla(QsciScintilla::SCI_AUTOCSETDROPRESTOFWORD, 1);
        editor_->setCallTipsStyle(QsciScintilla::CallTipsContext);
        editor_->setCallTipsVisible(8);

        QFont font("Menlo", 14);
        font.setStyleHint(QFont::Monospace);
        editor_->setFont(font);
        editor_->setMarginsFont(font);
        editor_->setPaper(QColor("#0d1117"));
        editor_->setColor(QColor("#e6edf3"));
        editor_->setCaretForegroundColor(QColor("#ffd866"));
        editor_->setCaretLineBackgroundColor(QColor("#161b22"));
        editor_->SendScintilla(QsciScintilla::SCI_SETMULTIPLESELECTION, 1);
        editor_->SendScintilla(QsciScintilla::SCI_SETADDITIONALSELECTIONTYPING, 1);
    }

    void configure_lexer() {
        QFont font("Menlo", 14);
        font.setStyleHint(QFont::Monospace);
        lexer_->setDefaultFont(font);
        lexer_->setDefaultPaper(QColor("#0d1117"));
        lexer_->setDefaultColor(QColor("#e6edf3"));
        lexer_->setColor(QColor("#ff7b72"), QsciLexerLua::Keyword);
        lexer_->setColor(QColor("#a5d6ff"), QsciLexerLua::String);
        lexer_->setColor(QColor("#8b949e"), QsciLexerLua::Comment);
        lexer_->setColor(QColor("#d2a8ff"), QsciLexerLua::Number);
        lexer_->setColor(QColor("#79c0ff"), QsciLexerLua::BasicFunctions);
        lexer_->setColor(QColor("#79c0ff"), QsciLexerLua::StringTableMathsFunctions);
        lexer_->setColor(QColor("#ffa657"), QsciLexerLua::Operator);
        lexer_->setPaper(QColor("#0d1117"));
    }

    void configure_api() {
        QStringList words{
            "function", "local", "if", "then", "elseif", "else", "end",
            "for", "while", "repeat", "until", "return", "break", "true",
            "false", "nil", "and", "or", "not",
            "do", "in", "goto",
            "assert", "collectgarbage", "dofile", "error", "getmetatable",
            "ipairs", "load", "loadfile", "next", "pairs", "pcall",
            "print", "rawequal", "rawget", "rawlen", "rawset", "require",
            "select", "setmetatable", "tonumber", "tostring", "type", "xpcall",
            "_G", "_VERSION",
            "coroutine.create", "coroutine.resume", "coroutine.running",
            "coroutine.status", "coroutine.wrap", "coroutine.yield",
            "math.abs", "math.acos", "math.asin", "math.atan", "math.ceil",
            "math.cos", "math.deg", "math.exp", "math.floor", "math.fmod",
            "math.huge", "math.log", "math.max", "math.min", "math.modf",
            "math.pi", "math.rad", "math.random", "math.randomseed",
            "math.sin", "math.sqrt", "math.tan",
            "string.byte", "string.char", "string.dump", "string.find",
            "string.format", "string.gmatch", "string.gsub", "string.len",
            "string.lower", "string.match", "string.rep", "string.reverse",
            "string.sub", "string.upper",
            "table.concat", "table.insert", "table.move", "table.pack",
            "table.remove", "table.sort", "table.unpack",
            "io.close", "io.flush", "io.input", "io.lines", "io.open",
            "io.output", "io.popen", "io.read", "io.tmpfile", "io.type",
            "io.write",
            "os.clock", "os.date", "os.difftime", "os.execute", "os.exit",
            "os.getenv", "os.remove", "os.rename", "os.setlocale", "os.time",
            "os.tmpname",
            "on_frame(frame)",
            "function on_frame(frame)\n    snes.clear_input()\n\nend",
            "snes.read8(endereco)",
            "snes.write8(endereco, valor)",
            "snes.read16(endereco)",
            "snes.write16(endereco, valor)",
            "snes.press(botao)",
            "snes.press(\"right\")",
            "snes.press(\"b\")",
            "snes.release(botao)",
            "snes.release(\"right\")",
            "snes.set_button(botao, ativo)",
            "snes.set_button(\"b\", true)",
            "snes.clear_input()",
            "snes.frame()",
            "snes.set_speed(multiplicador)",
            "snes.speed()",
            "snes.log(...)",
            "snes.log(\"frame\", frame)",
            "snes.save_state()",
            "snes.load_state()",
            "snes.draw_text(x, y, texto, r, g, b, a, escala)",
            "snes.draw_rect(x, y, largura, altura, r, g, b, a, preenchido)",
            "snes.draw_line(x1, y1, x2, y2, r, g, b, a)",
            "snes.clear_overlay()",
            "read8", "write8", "read16", "write16", "press", "release",
            "set_button", "clear_input", "frame", "set_speed", "speed", "log", "save_state",
            "load_state", "draw_text", "draw_rect", "draw_line",
            "clear_overlay",
            "0x7E0000", "0x7E0019", "0x700000",
            "RETRO_MEMORY_SYSTEM_RAM", "WRAM", "VRAM", "SRAM",
            "up", "down", "left", "right", "a", "b", "x", "y", "l", "r",
            "start", "select",
            "\"up\"", "\"down\"", "\"left\"", "\"right\"",
            "\"a\"", "\"b\"", "\"x\"", "\"y\"",
            "\"l\"", "\"r\"", "\"start\"", "\"select\"",
        };
        words.sort(Qt::CaseInsensitive);
        words.removeDuplicates();
        for (const auto &word : words) {
            apis_->add(word);
        }
    }

    void add_document_words(const QString &text) {
        QString current;
        QStringList words;
        for (const QChar ch : text) {
            if (ch.isLetterOrNumber() || ch == '_') {
                current.append(ch);
            } else {
                if (current.size() > 2) {
                    words.append(current);
                }
                current.clear();
            }
        }
        if (current.size() > 2) {
            words.append(current);
        }
        words.removeDuplicates();
        for (const auto &word : words) {
            apis_->add(word);
        }
    }

    void save() {
        QSaveFile file(path_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "SNES Lua Studio",
                                 "Nao foi possivel salvar o script.");
            return;
        }
        file.write(editor_->text().toUtf8());
        if (!file.commit()) {
            QMessageBox::warning(this, "SNES Lua Studio",
                                 "Nao foi possivel finalizar o salvamento.");
            return;
        }
        statusBar()->showMessage("Salvo: " + path_, 2500);
    }

    QString path_;
    QsciScintilla *editor_ = nullptr;
    QsciLexerLua *lexer_ = nullptr;
    QsciAPIs *apis_ = nullptr;
};

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("scripts/novo-script.lua");
    LuaEditorWindow window(path);
    window.show();
    return app.exec();
}

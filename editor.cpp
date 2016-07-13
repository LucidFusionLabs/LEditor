/*
 * $Id$
 * Copyright (C) 2009 Lucid Fusion Labs

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "core/app/gui.h"
#include "core/app/ipc.h"
#include "core/app/bindings/ide.h"
#include "core/app/bindings/syntax.h"
#include "core/imports/diff-match-patch-cpp-stl/diff_match_patch.h"

namespace LFL {
DEFINE_string(project,         "",              "CMake build dir");
DEFINE_int   (width,           840,             "Window width");
DEFINE_int   (height,          760,             "Window height");
DEFINE_string(llvm_dir,        "/Users/p/llvm", "LLVM toolchain");
DEFINE_string(cvs_cmd,         "git",           "CVS command, ie git");
DEFINE_string(cmake_daemon,    "bin/cmake",     "CMake daemon");
DEFINE_string(default_project, "",              "Default project");
DEFINE_bool  (clang,           true,            "Use libclang");
DEFINE_bool  (clang_highlight, false,           "Use Clang syntax matcher");
DEFINE_bool  (regex_highlight, true,            "Use Regex syntax matcher");

extern FlagOfType<bool> FLAGS_enable_network_;

struct MyAppState {
  vector<string> save_settings = { "default_project" };
  unique_ptr<IDEProject> project;
  SearchPaths search_paths;
  string build_bin;
  Editor::SyntaxColors *cpp_colors = Singleton<Editor::Base16DefaultDarkSyntaxColors>::Get();
  unique_ptr<SystemMenuWidget> file_menu, edit_menu, view_menu;
  unique_ptr<SystemPanelWidget> find_panel, gotoline_panel;

  MyAppState() : search_paths(getenv("PATH")), build_bin(search_paths.Find("make")) {}
} *my_app;

struct MyEditorDialog : public EditorDialog {
  unique_ptr<TranslationUnit> main_tu, next_tu;
  vector<DrawableAnnotation> main_annotation, next_annotation;
  vector<pair<int, int>> find_results;
  bool parsing=0, needs_reparse=0;
  int file_type=0, reparsed=0, find_results_ind=0;
  SyntaxMatcher *regex_highlighter=0;
  using EditorDialog::EditorDialog;
  virtual ~MyEditorDialog() {}
};

struct EditorGUI : public GUI {
  static const int init_right_divider_w=224;
  Box top_center_pane, bottom_center_pane, left_pane, right_pane;
  Widget::Divider bottom_divider, right_divider;
  unordered_map<string, shared_ptr<MyEditorDialog>> opened_files;
  TabbedDialogInterface *right_pane_tabs=0;
  TabbedDialog<MyEditorDialog> source_tabs;
  TabbedDialog<Dialog> project_tabs;
  TabbedDialog<PropertyTreeDialog> options_tabs;
  DirectoryTreeDialog dir_tree;
  PropertyTreeDialog targets_tree, options_tree;
  CodeCompletionsViewDialog code_completions;
  MyEditorDialog *code_completions_editor=0;
  unique_ptr<Terminal> build_terminal;
  vector<MenuItem> source_context_menu{ MenuItem{ "", "Go To Brace", "gotobrace" },
    MenuItem{ "", "Go To Definition", "gotodef" } };
  vector<MenuItem> dir_context_menu{ MenuItem{ "b", "Build", "build" } };
  bool console_animating = 0;
  ProcessPipe build_process;
  CMakeDaemon cmakedaemon;
  CMakeDaemon::TargetInfo default_project;
  RegexCPlusPlusHighlighter cpp_highlighter;
  RegexCMakeHighlighter cmake_highlighter;

  EditorGUI(Window *W) : GUI(W),
    bottom_divider(this, true, 0), right_divider(this, false, init_right_divider_w),
    source_tabs(this), project_tabs(this), options_tabs(this),
    dir_tree        (screen, app->fonts->Change(screen->default_font, 0, Color::black, Color::grey90)),
    targets_tree    (screen, app->fonts->Change(screen->default_font, 0, Color::black, Color::grey90)),
    options_tree    (screen, app->fonts->Change(screen->default_font, 0, Color::black, Color::grey90)),
    code_completions(screen, app->fonts->Change(screen->default_font, 0, *my_app->cpp_colors->GetFGColor("StatusLine"), *my_app->cpp_colors->GetBGColor("StatusLine"))),
    cpp_highlighter  (my_app->cpp_colors, my_app->cpp_colors->SetDefaultAttr(0)),
    cmake_highlighter(my_app->cpp_colors, my_app->cpp_colors->SetDefaultAttr(0)) {
    Activate(); 

    dir_tree.deleted_cb = [&](){ right_divider.size=0; right_divider.changed=1; };
    if (my_app->project) dir_tree.title_text = "Source";
    if (my_app->project) dir_tree.view.Open(StrCat(my_app->project->source_dir, LocalFile::Slash));
    dir_tree.view.InitContextMenu(bind([=](){ app->ShowSystemContextMenu(dir_context_menu); }));
    dir_tree.view.selected_line_clicked_cb = [&](PropertyView *v, PropertyTree::Id id) {
      if (auto n = v->GetNode(id)) if (n->val.size() && n->val.back() != '/') Open(n->val);
    };
    project_tabs.AddTab(&dir_tree);
    screen->gui.push_back(&dir_tree);

    targets_tree.deleted_cb = [&](){ right_divider.size=0; right_divider.changed=1; };
    if (my_app->project) targets_tree.title_text = "Targets";
    project_tabs.AddTab(&targets_tree);
    screen->gui.push_back(&targets_tree);
    project_tabs.SelectTab(&dir_tree);

    options_tree.view.SetRoot(options_tree.view.AddNode(nullptr, "", PropertyTree::Children{
      options_tree.view.AddNode(nullptr, "Aaaa", PropertyTree::Children{
        options_tree.view.AddNode(nullptr, "A-sub1"), options_tree.view.AddNode(nullptr, "A-sub2"),
        options_tree.view.AddNode(nullptr, "A-sub3"), options_tree.view.AddNode(nullptr, "A-sub4")}),
      options_tree.view.AddNode(nullptr, "Bb", PropertyTree::Children{
        options_tree.view.AddNode(nullptr, "B-sub1"), options_tree.view.AddNode(nullptr, "B-sub2"),
        options_tree.view.AddNode(nullptr, "B-sub3"), options_tree.view.AddNode(nullptr, "B-sub4")}) }));
    options_tree.deleted_cb = [&](){ right_divider.size=0; right_divider.changed=1; };
    if (my_app->project) options_tree.title_text = "Options";
    options_tabs.AddTab(&options_tree);
    screen->gui.push_back(&options_tree);

    code_completions.deleted_cb = [=](){ code_completions_editor = nullptr; };
    screen->gui.push_back(&code_completions);

    build_terminal = make_unique<Terminal>(nullptr, screen, screen->default_font);
    build_terminal->newline_mode = true;

    if (my_app->project && !FLAGS_cmake_daemon.empty()) {
      cmakedaemon.init_targets_cb = [&](){ app->RunInMainThread([&]{
        targets_tree.view.tree.Clear();
        PropertyTree::Children target;
        for (auto &t : cmakedaemon.targets) target.push_back(targets_tree.view.AddNode(nullptr, t.first));
        targets_tree.view.SetRoot(targets_tree.view.AddNode(nullptr, "", move(target)));
        targets_tree.view.Reload();
        targets_tree.view.Redraw();
        if (!FLAGS_default_project.empty() && !cmakedaemon.GetTargetInfo
            (FLAGS_default_project, bind(&EditorGUI::UpdateDefaultProjectProperties, this, _1)))
          ERROR("default_project ", FLAGS_default_project, " not found");
      }); };
      cmakedaemon.Start(Asset::FileName(FLAGS_cmake_daemon), my_app->project->build_dir);
    }
  }

  MyEditorDialog *Top() { return source_tabs.top; }

  MyEditorDialog *Open(const string &fin) {
    static string prefix = "file://";
    string fn = PrefixMatch(fin, prefix) ? fin.substr(prefix.size()) : fin;
    auto opened = opened_files.find(fn);
    if (opened != opened_files.end()) {
      source_tabs.SelectTab(opened->second.get());
      return opened->second.get();
    }
    INFO("Editor Open ", fn);
    return OpenFile(new LocalFile(fn, "r"));
  }

  MyEditorDialog *OpenFile(File *input_file) {
    MyEditorDialog *editor = new MyEditorDialog(screen, screen->default_font, input_file, 1, 1); 
    Editor *e = &editor->view;
    string fn = e->file->Filename();
    opened_files[fn] = shared_ptr<MyEditorDialog>(editor);
    if      (FileSuffix::CPP(fn))   editor->file_type = FileType::CPP;
    else if (FileSuffix::CMake(fn)) editor->file_type = FileType::CMake;
    if (FLAGS_regex_highlight) switch(editor->file_type) {
      case FileType::CPP:   editor->regex_highlighter = &cpp_highlighter;   break;
      case FileType::CMake: editor->regex_highlighter = &cmake_highlighter; break;
    }
    source_tabs.AddTab(editor);
    child_box.Clear();

    e->UpdateMapping(0, FLAGS_regex_highlight);
    if (my_app->project && FLAGS_clang) ParseTranslationUnit(FindOrDie(opened_files, e->file->Filename())); 
    e->line.SetAttrSource(&e->style);
    e->SetColors(my_app->cpp_colors);
    e->InitContextMenu(bind([=](){ app->ShowSystemContextMenu(source_context_menu); }));
    e->modified_cb = [=]{ app->scheduler.WakeupIn(0, Seconds(1), true); };
    e->newline_cb = bind(&EditorGUI::IndentNewline, this, editor);
    e->tab_cb = bind(&EditorGUI::CompleteCode, this);

    if (editor->file_type) {
      e->annotation_cb = [=](const Editor::LineMap::Iterator &i, const String16 &t,
                             bool first_line, int check_shift, int shift_offset){
        if (i.val->annotation_ind < 0) i.val->annotation_ind = PushBackIndex(editor->main_annotation, DrawableAnnotation());
        if (editor->regex_highlighter) {
          DrawableAnnotation annotation;
          if (check_shift) swap(annotation, editor->main_annotation[i.val->annotation_ind]);
          editor->regex_highlighter->GetLineAnnotation
            (e, i, t, first_line, &e->syntax_parsed_line_index, &e->syntax_parsed_anchor, &editor->main_annotation[0]);
          if (annotation.Shifted(editor->main_annotation[i.val->annotation_ind], check_shift, shift_offset)) return NullPointer<DrawableAnnotation>();
        }
        return i.val->annotation_ind < 0 ? nullptr : &editor->main_annotation[i.val->annotation_ind];
      };
      if (editor->regex_highlighter) {
        editor->main_annotation.resize(e->file_line.size());
        // editor->regex_highlighter->UpdateAnnotation(e, &editor->main_annotation[0], editor->main_annotation.size());
      }
    }

    if (source_tabs.box.h) e->CheckResized(Box(source_tabs.box.w, source_tabs.box.h-source_tabs.tab_dim.y));
    editor->deleted_cb = [=](){ source_tabs.DelTab(editor); child_box.Clear(); opened_files.erase(fn); };
    return editor;
  }

  TranslationUnit::OpenedFiles MakeOpenedFilesVector() const {
    TranslationUnit::OpenedFiles opened;
    for (auto &i : opened_files)
      if (i.second->view.CacheModifiedText())
        opened.emplace_back(i.first, i.second->view.cached_text);
    return opened;
  }

  void Layout() {
    ResetGL();
    box = screen->Box();
    right_divider.LayoutDivideRight(box, &top_center_pane, &right_pane, -box.h);
    bottom_divider.LayoutDivideBottom(top_center_pane, &top_center_pane, &bottom_center_pane, -box.h);
    source_tabs.box = top_center_pane;
    source_tabs.tab_dim.y = screen->default_font->Height();
    source_tabs.Layout();
    if (1) right_pane_tabs = &project_tabs;
    else   right_pane_tabs = &options_tabs;
    right_pane_tabs->box = right_pane;
    right_pane_tabs->tab_dim.y = screen->default_font->Height();
    right_pane_tabs->Layout();
    if (!child_box.Size()) child_box.PushNop();
  }

  int Frame(LFL::Window *W, unsigned clicks, int flag) {
    if (Singleton<FlagMap>::Get()->dirty) SettingsFile::Save(my_app->save_settings);
    Time now = Now();
    MyEditorDialog *d = Top();
    GraphicsContext gc(W->gd);
    if (d && d->view.modified != Time(0) && d->view.modified + Seconds(1) <= now) {
      d->view.modified = Time(0); 
      if (FLAGS_clang) ReparseTranslationUnit(FindOrDie(opened_files, d->view.file->Filename())); 
    }

    gc.gd->DisableBlend();
    if (bottom_divider.changed || right_divider.changed) Layout();
    if (child_box.data.empty()) Layout();
    source_tabs.Draw();
    GUI::Draw();
    gc.gd->DrawMode(DrawMode::_2D);
    if (bottom_center_pane.h) build_terminal->Draw(bottom_center_pane, TextArea::DrawFlag::CheckResized);
    gc.gd->DrawMode(DrawMode::_2D);
    if (right_pane.w) right_pane_tabs->Draw();
    if (right_divider.changing) BoxOutline().Draw(&gc, Box::DelBorder(right_pane, Border(1,1,1,1)));
    if (bottom_divider.changing) BoxOutline().Draw(&gc, Box::DelBorder(bottom_center_pane, Border(1,1,1,1)));
    if (code_completions_editor == d) code_completions.Draw();
    W->DrawDialogs();
    return 0;
  }

  void UpdateAnimating() { app->scheduler.SetAnimating(screen, console_animating); }
  void OnConsoleAnimating() { console_animating = screen->console->animating; UpdateAnimating(); }
  void ShowProjectExplorer() { right_divider.size = init_right_divider_w; right_divider.changed=1; }
  void ShowBuildTerminal() { bottom_divider.size = screen->default_font->Height()*5; bottom_divider.changed=1; }
  void UpdateDefaultProjectProperties(const CMakeDaemon::TargetInfo &v) { default_project = v; }

  void IndentNewline(MyEditorDialog *e) {
    // find first previous line with text and call its indentation x
    // next identation is x + abs_bracks
  }
  
  void ParseTranslationUnit(shared_ptr<MyEditorDialog> d) {
    string filename = d->view.file->Filename(), compile_cmd, compile_dir;
    if (!my_app->project->GetCompileCommand(filename, &compile_cmd, &compile_dir)) {
      if (!FileSuffix::CPP(filename)) return ERROR("no compile command for ", filename);
      compile_cmd = "clang";
      compile_dir = default_project.output.substr(0, DirNameLen(default_project.output));
      for (auto &d : default_project.compile_definitions) StrAppend(&compile_cmd, " -D", d);
      for (auto &o : default_project.compile_options)     StrAppend(&compile_cmd, " ",   o);
      for (auto &i : default_project.include_directories) StrAppend(&compile_cmd, " -I", i);
      StrAppend(&compile_cmd, "-c src.c -o out.o");
    }
    ParseTranslationUnit(d, new TranslationUnit(filename, compile_cmd, compile_dir), false);
  }

  void ParseTranslationUnit(shared_ptr<MyEditorDialog> d, TranslationUnit *tu, bool reparse) {
    d->reparsed++;
    d->parsing = true;
    d->needs_reparse = false;
    auto opened = MakeOpenedFilesVector();
    for (auto i = d->view.file_line.Begin(); i.ind; ++i) i.val->next_tu_line = i.GetIndex();
    app->RunInThreadPool([=](){
      if (reparse) tu->Reparse(opened);
      else         tu->Parse(opened);
      if (FLAGS_clang_highlight)
        ClangCPlusPlusHighlighter::UpdateAnnotation(tu, my_app->cpp_colors, d->view.default_attr, &d->next_annotation);
      app->RunInMainThread([=](){ HandleParseTranslationUnitDone(d, tu, !reparse); });
    });
  }

  void ReparseTranslationUnit(shared_ptr<MyEditorDialog> d) {
    if (d->parsing) { d->needs_reparse = true; return; }
    if (d->next_tu && d->next_tu->parse_failed) d->next_tu.reset();
    if (!d->next_tu) ParseTranslationUnit(d);
    else             ParseTranslationUnit(d, d->next_tu.get(), true);
  }

  void HandleParseTranslationUnitDone(shared_ptr<MyEditorDialog> d, TranslationUnit *tu, bool replace) {
    if (!app->run) return;
    swap(d->main_tu, d->next_tu);
    if (FLAGS_clang_highlight) swap(d->main_annotation, d->next_annotation);
    if (replace) d->main_tu = unique_ptr<TranslationUnit>(tu);
    for (auto i = d->view.file_line.Begin(); i.ind; ++i) {
      if (i.val->next_tu_line >= 0) {
        i.val->main_tu_line = i.val->next_tu_line;
        if (FLAGS_clang_highlight) i.val->annotation_ind = i.val->main_tu_line;
      }
      i.val->next_tu_line =-1;
    }
    if (FLAGS_clang_highlight) {
      d->view.RefreshLines();
      d->view.Redraw();
    }
    d->parsing = false;
  }

  void CompleteCode() {
    MyEditorDialog *d = Top();
    if (!d) return;
    if (d == code_completions_editor) return code_completions.deleted_cb();
    if (d->file_type == FileType::CPP) {
      if (!d->view.cursor_offset || !d->main_tu) return;
      code_completions.view.completions = d->main_tu->CompleteCode
        (MakeOpenedFilesVector(), d->view.cursor_line_index, d->view.cursor.i.x);
    } else if (d->file_type == FileType::CMake) {
      if (!d->view.cursor_offset || !cmakedaemon.Ready()) return;
      d->view.CacheModifiedText(true);
      code_completions.view.completions = cmakedaemon.CompleteCode
        (d->view.file->Filename(), d->view.cursor_line_index, d->view.cursor.i.x, d->view.cached_text->buf);
    } else return;
    if (!code_completions.view.completions) return;
    code_completions.view.RefreshLines();
    code_completions.view.Redraw();
    point dim(d->view.style.font->max_width*20, d->view.style.font->Height()*10);
    code_completions.box = Box(d->view.cursor.p - point(0, dim.y), dim);
    code_completions_editor = d;
  }

  void GotoMatchingBrace() {
    MyEditorDialog *d = Top();
    if (!d || !d->main_tu || !d->view.cursor_offset || d->view.cursor_offset->main_tu_line < 0) return;
    auto r = d->main_tu->GetCursorExtent(d->view.file->Filename(), d->view.cursor_offset->main_tu_line, d->view.cursor.i.x);
    if (IsOpenParen(d->view.CursorGlyph())) d->view.ScrollTo(r.second.y-1, r.second.x-1);
    else                                    d->view.ScrollTo(r.first .y-1, r.first .x-1);
  }

  void GotoDefinition() {
    MyEditorDialog *d = Top();
    if (!d || !d->main_tu || !d->view.cursor_offset || d->view.cursor_offset->main_tu_line < 0) return;
    auto fo = d->main_tu->FindDefinition(d->view.file->Filename(), d->view.cursor_offset->main_tu_line, d->view.cursor.i.x);
    if (fo.fn.empty()) return;
    auto editor = Open(fo.fn);
    editor->view.ScrollTo(fo.y-1, fo.x-1);
  }

  void GotoLine(const string &line) {
    if (line.empty()) my_app->gotoline_panel->Show();
    else if (MyEditorDialog *d = Top()) d->view.ScrollTo(atoi(line)-1, 0);
  }

  void Find(const string &line) {
    MyEditorDialog *d = Top();
    if (line.empty() || !d) return my_app->find_panel->Show();
    CHECK(d->view.CacheModifiedText(true));
    d->find_results.clear();
    Regex regex(line);
    RegexLineMatcher(&regex, d->view.cached_text->buf).MatchAll(&d->find_results);
    if (!d->find_results.size()) return my_app->find_panel->SetTitle("Find");
    d->find_results_ind = -1;
    FindPrevOrNext(false);
  }

  void FindPrevOrNext(bool prev) {
    MyEditorDialog *d = Top();
    if (!d || !d->find_results.size()) return;
    d->find_results_ind = RingIndex::Wrap(d->find_results_ind + (prev ? -1 : 1), d->find_results.size());
    const auto &r = d->find_results[d->find_results_ind];
    d->view.ScrollTo(r.first, r.second);
    my_app->find_panel->SetTitle(StrCat("Find [", d->find_results_ind+1, " of ", d->find_results.size(), "]"));
  }

  void Build() {
    if (bottom_divider.size < screen->default_font->Height()) ShowBuildTerminal();
    if (build_process.in) return;
    vector<const char*> argv{ my_app->build_bin.c_str(), nullptr };
    string build_dir = StrCat(my_app->project->build_dir, LocalFile::Slash, "term");
    CHECK(!build_process.Open(&argv[0], build_dir.c_str()));
    app->RunInNetworkThread([=](){ app->net->unix_client->AddConnectedSocket
      (fileno(build_process.in), new Connection::CallbackHandler
       ([=](Connection *c){ build_terminal->Write(c->rb.buf); c->ReadFlush(c->rb.size()); },
        [=](Connection *c){ build_process.Close(); })); });
  }

  void Tidy() {
    MyEditorDialog *d = Top();
    if (bottom_divider.size < screen->default_font->Height()) ShowBuildTerminal();
    if (build_process.in || !d) return;
    string tidy_bin = StrCat(FLAGS_llvm_dir, "/bin/clang-tidy"), src_file = d->view.file->Filename(),
           build_dir = my_app->project->build_dir;
    vector<const char*> argv{ tidy_bin.c_str(), "-p", build_dir.c_str(), src_file.c_str(), nullptr };
    CHECK(!build_process.Open(&argv[0], build_dir.c_str()));
    app->RunInNetworkThread([=](){ app->net->unix_client->AddConnectedSocket
      (fileno(build_process.in), new Connection::CallbackHandler
       ([=](Connection *c){ build_terminal->Write(c->rb.buf); c->ReadFlush(c->rb.size()); },
        [=](Connection *c){ build_process.Close(); })); });
  }

  void DiffUnsavedChanges() {
    MyEditorDialog *d = Top();
    if (!d) return;
    BufferFile current(string(""));
    d->view.SaveTo(&current);
    diff_match_patch<string> dmp;
    OpenFile(new BufferFile(dmp.patch_toText(dmp.patch_make(d->view.file->Contents(), current.buf)),
                            StrCat(d->view.file->Filename(), ".diff").c_str()));
  }

  void DiffCVS() {
  }
};

void MyWindowInit(Window *W) {
  W->width = FLAGS_width;
  W->height = FLAGS_height;
  W->caption = app->name;
  CHECK_EQ(0, W->NewGUI());
}

void MyWindowStart(Window *W) {
  EditorGUI *editor_gui = W->ReplaceGUI(0, make_unique<EditorGUI>(W));
  if (FLAGS_console) W->InitConsole(bind(&EditorGUI::OnConsoleAnimating, editor_gui));
  W->frame_cb = bind(&EditorGUI::Frame, editor_gui, _1, _2, _3);
  W->default_textbox = [=](){ auto t = editor_gui->Top(); return t ? &t->view : nullptr; };

  W->shell = make_unique<Shell>();
  W->shell->Add("choose",       [=](const vector<string>&) { app->ShowSystemFileChooser(1,0,0,"open"); });
  W->shell->Add("save",         [=](const vector<string>&) { if (auto t = editor_gui->Top()) t->view.Save();                        app->scheduler.Wakeup(0); });
  W->shell->Add("wrap",         [=](const vector<string>&a){ if (auto t = editor_gui->Top()) t->view.SetWrapMode(a.size()?a[0]:""); app->scheduler.Wakeup(0); });
  W->shell->Add("undo",         [=](const vector<string>&) { if (auto t = editor_gui->Top()) t->view.WalkUndo(true);                app->scheduler.Wakeup(0); });
  W->shell->Add("redo",         [=](const vector<string>&) { if (auto t = editor_gui->Top()) t->view.WalkUndo(false);               app->scheduler.Wakeup(0); });
  W->shell->Add("gotobrace",    [=](const vector<string>&a){ editor_gui->GotoMatchingBrace();       app->scheduler.Wakeup(0); });
  W->shell->Add("gotodef",      [=](const vector<string>&a){ editor_gui->GotoDefinition();          app->scheduler.Wakeup(0); });
  W->shell->Add("open",         [=](const vector<string>&a){ editor_gui->Open(a.size() ? a[0]: ""); app->scheduler.Wakeup(0); });
  W->shell->Add("gotoline",     [=](const vector<string>&a){ editor_gui->GotoLine(a.size()?a[0]:"");app->scheduler.Wakeup(0); });
  W->shell->Add("find",         [=](const vector<string>&a){ editor_gui->Find(a.size() ? a[0]: ""); app->scheduler.Wakeup(0); });
  W->shell->Add("findprev",     [=](const vector<string>&) { editor_gui->FindPrevOrNext(true);      app->scheduler.Wakeup(0); });
  W->shell->Add("findnext",     [=](const vector<string>&) { editor_gui->FindPrevOrNext(false);     app->scheduler.Wakeup(0); });
  W->shell->Add("build",        [=](const vector<string>&) { editor_gui->Build();                   app->scheduler.Wakeup(0); });
  W->shell->Add("tidy",         [=](const vector<string>&) { editor_gui->Tidy();                    app->scheduler.Wakeup(0); });
  W->shell->Add("show_project", [=](const vector<string>&) { editor_gui->ShowProjectExplorer();     app->scheduler.Wakeup(0); });
  W->shell->Add("show_build",   [=](const vector<string>&) { editor_gui->ShowBuildTerminal();       app->scheduler.Wakeup(0); });
  W->shell->Add("diff_unsaved", [=](const vector<string>&) { editor_gui->DiffUnsavedChanges();      app->scheduler.Wakeup(0); });
  W->shell->Add("diff_cvs",     [=](const vector<string>&) { editor_gui->DiffCVS();                 app->scheduler.Wakeup(0); });

  BindMap *binds = W->AddInputController(make_unique<BindMap>());
  binds->Add('6', Key::Modifier::Cmd, Bind::CB(bind(&Shell::console, W->shell.get(), vector<string>())));
}

}; // namespace LFL
using namespace LFL;

extern "C" void MyAppCreate(int argc, const char* const* argv) {
  FLAGS_enable_video = FLAGS_enable_input = true;
  FLAGS_threadpool_size = 1;
  app = new Application(argc, argv);
  screen = new Window();
  my_app = new MyAppState();
  app->name = "LEdit";
  app->window_start_cb = MyWindowStart;
  app->window_init_cb = MyWindowInit;
  app->window_init_cb(screen);
  app->exit_cb = [](){ delete my_app; };
}

extern "C" int MyAppMain() {
  if (app->Create(__FILE__)) return -1;
  SettingsFile::Load();
  screen->width = FLAGS_width;
  screen->height = FLAGS_height;

  if (app->Init()) return -1;
  int optind = Singleton<FlagMap>::Get()->optind;
  if (optind >= app->argc) { fprintf(stderr, "Usage: %s [-flags] <file>\n", app->argv[0]); return -1; }

  app->scheduler.AddFrameWaitKeyboard(screen);
  app->scheduler.AddFrameWaitMouse(screen);

  bool start_network_thread = !(FLAGS_enable_network_.override && !FLAGS_enable_network);
  if (start_network_thread) {
    app->net = make_unique<Network>();
    CHECK(app->CreateNetworkThread(false, true));
  }

  my_app->file_menu = make_unique<SystemMenuWidget>("File", vector<MenuItem>{ MenuItem{"o", "Open", "choose"}, MenuItem{"s", "Save", "save" },
    MenuItem{"b", "Build", "build"}, MenuItem{"", "Tidy", "tidy"} });

  my_app->edit_menu = SystemMenuWidget::CreateEditMenu({ MenuItem{"z", "Undo", "undo"}, MenuItem{"y", "Redo", "redo"},
    MenuItem{"f", "Find", "find"}, MenuItem{"g", "Goto", "gotoline"}, MenuItem{"", "Diff unsaved", "diff_unsaved"},
    MenuItem{"", StrCat(FLAGS_cvs_cmd, " diff"), "diff_cvs"} });

  my_app->view_menu = make_unique<SystemMenuWidget>("View", vector<MenuItem>{ MenuItem{"=", "Zoom In", ""}, MenuItem{"-", "Zoom Out", ""},
    MenuItem{"", "No wrap", "wrap none"}, MenuItem{"", "Line wrap", "wrap lines"}, MenuItem{"", "Word wrap", "wrap words"}, 
    MenuItem{"", "Show Project Explorer", "show_project"}, MenuItem{"", "Show Build Console", "show_build"} });

  my_app->find_panel = make_unique<SystemPanelWidget>(Box(0, 0, 300, 60), "Find", vector<PanelItem>{
    PanelItem{ "textbox", Box(20, 20, 160, 20), "find" }, 
    PanelItem{ "button:<", Box(200, 20, 40, 20), "findprev" },
    PanelItem{ "button:>", Box(240, 20, 40, 20), "findnext" }
  });

  my_app->gotoline_panel = make_unique<SystemPanelWidget>(Box(0, 0, 200, 60), "Goto line number", vector<PanelItem>{
    PanelItem{ "textbox", Box(20, 20, 160, 20), "gotoline" } });

  if (FLAGS_project.size()) {
    my_app->project = make_unique<IDEProject>(FLAGS_project);
    INFO("Project dir = ", my_app->project->build_dir);
    INFO("Found make = ", my_app->build_bin);
    INFO("Default project = ", FLAGS_default_project);
  }

  app->StartNewWindow(screen);
  screen->gd->ClearColor(Color::grey70);
  EditorGUI *editor_gui = screen->GetOwnGUI<EditorGUI>(0);

  editor_gui->Open(app->argv[optind]);
  return app->Main();
}

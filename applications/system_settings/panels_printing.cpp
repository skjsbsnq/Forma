// Printers (CUPS) and Locations (xdg-user-dirs, in panels_desktop.cpp): system data with no
// desktop-specific backend, so GNOME and Plasma share them.
//
// libcups is dlopen()ed like libdbus / libwayland-client elsewhere in the project: printing settings are
// useless on a machine without CUPS, and the app must still build and run there. cups_dest_t / cups_job_t
// are plain C structs, so a local copy of their layout is all that's needed.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "mayonnaise/ui_translation.hpp"

#include "glib_support.hpp"
#include "panels.hpp"
#include "ui.hpp"

using namespace mayo;

namespace {

namespace cups {

enum class PrinterState : int { Idle = 3, Processing = 4, Stopped = 5 };
enum class JobState : int {
    Pending = 3,
    Held = 4,
    Processing = 5,
    Stopped = 6,
    Canceled = 7,
    Aborted = 8,
    Completed = 9
};

struct Option {
    char* name;
    char* value;
};

struct Dest {
    char* name;
    char* instance;
    int isDefault;
    int numOptions;
    Option* options;
};

struct Job {
    int id;
    char* dest;
    char* title;
    char* user;
    char* format;
    int state;
    int size;
    int priority;
    long completedTime;
    long creationTime;
    long processingTime;
};

/// The libcups surface the panel uses, loaded once. CUPS's admin operations (pause, accept/reject) have no
/// stable library entry point, so those go through `lpadmin` exactly like Plasma's print-manager does.
struct Lib {
    int (*getDests)(Dest** dests) = nullptr;
    void (*freeDests)(int num, Dest* dests) = nullptr;
    const char* (*getDefault)() = nullptr;
    void (*setDefaultDest)(const char* name, const char* instance) = nullptr;
    int (*getJobs)(Job** jobs, const char* name, int myJobs, int whichJobs) = nullptr;
    void (*freeJobs)(int num, Job* jobs) = nullptr;
    int (*cancelJob)(const char* name, int id) = nullptr;
    const char* (*getOption)(const char* name, int num, Option* options) = nullptr;

    [[nodiscard]] bool ok() const {
        return getDests && freeDests && getDefault && setDefaultDest && getJobs && freeJobs && cancelJob && getOption;
    }

    static const Lib* get() {
        static const Lib* instance = []() -> const Lib* {
            static Lib lib;
            // libcups.so.2 is the runtime name on every distribution; the unversioned name only exists with
            // the -dev package, so try both (the versioned one first).
            void* h = dlopen("libcups.so.2", RTLD_NOW | RTLD_LOCAL);
            if (!h) h = dlopen("libcups.so", RTLD_NOW | RTLD_LOCAL);
            if (!h) return nullptr;
            auto sym = [h](auto& fn, const char* name) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name));
            };
            sym(lib.getDests, "cupsGetDests");
            sym(lib.freeDests, "cupsFreeDests");
            sym(lib.getDefault, "cupsGetDefault");
            sym(lib.setDefaultDest, "cupsSetDefaultDest");
            sym(lib.getJobs, "cupsGetJobs");
            sym(lib.freeJobs, "cupsFreeJobs");
            sym(lib.cancelJob, "cupsCancelJob");
            sym(lib.getOption, "cupsGetOption");
            return lib.ok() ? &lib : nullptr;
        }();
        return instance;
    }

    /// `name=value` from a destination's option array; empty when absent.
    [[nodiscard]] std::string option(const Dest& d, const char* key) const {
        const char* v = getOption(key, d.numOptions, d.options);
        return v ? v : "";
    }
};

struct Printer {
    std::string name;
    std::string message;
    std::string makeModel;
    PrinterState state = PrinterState::Idle;
    bool isDefault = false;
    bool accepting = true;
};

std::vector<Printer> printers() {
    std::vector<Printer> out;
    const Lib* lib = Lib::get();
    if (!lib) return out;
    Dest* dests = nullptr;
    const int n = lib->getDests(&dests);
    if (n <= 0 || !dests) return out;
    for (int i = 0; i < n; ++i) {
        const Dest& d = dests[i];
        if (!d.name || !*d.name) continue;
        Printer p;
        p.name = d.name;
        p.message = lib->option(d, "printer-state-message");
        p.makeModel = lib->option(d, "printer-make-and-model");
        // CUPS stores IPP booleans in the option list as "true"/"false".
        if (const std::string a = lib->option(d, "printer-is-accepting-jobs"); !a.empty()) p.accepting = a == "true";
        if (const std::string s = lib->option(d, "printer-state"); !s.empty())
            p.state = static_cast<PrinterState>(std::atoi(s.c_str()));
        p.isDefault = d.isDefault != 0;
        out.push_back(std::move(p));
    }
    lib->freeDests(n, dests);
    // The default printer first, then by name, like KDE's printer list.
    std::sort(out.begin(), out.end(), [](const Printer& a, const Printer& b) {
        if (a.isDefault != b.isDefault) return a.isDefault;
        return ui::lower(a.name) < ui::lower(b.name);
    });
    return out;
}

/// Copies the strings out of the C structs before cupsFreeJobs releases them.
struct JobInfo {
    int id;
    std::string dest;
    std::string title;
    int state;
};

std::vector<JobInfo> jobs(const std::string& printer) {
    std::vector<JobInfo> out;
    const Lib* lib = Lib::get();
    if (!lib) return out;
    Job* list = nullptr;
    // whichjobs: CUPS_WHICHJOBS_ACTIVE = 0; name nullptr means every destination.
    const int n = lib->getJobs(&list, printer.empty() ? nullptr : printer.c_str(), 0, 0);
    if (n <= 0 || !list) return out;
    for (int i = 0; i < n; ++i) {
        out.push_back(
            {list[i].id, list[i].dest ? list[i].dest : "", list[i].title ? list[i].title : "", list[i].state});
    }
    lib->freeJobs(n, list);
    return out;
}

void cancel(int id, const std::string& printer) {
    const Lib* lib = Lib::get();
    if (!lib || gx::readOnly()) return;
    lib->cancelJob(printer.empty() ? nullptr : printer.c_str(), id);
}

void makeDefault(const std::string& name) {
    const Lib* lib = Lib::get();
    if (!lib || gx::readOnly()) return;
    lib->setDefaultDest(name.c_str(), nullptr);
}

std::string currentDefault() {
    const Lib* lib = Lib::get();
    const char* name = lib ? lib->getDefault() : nullptr;
    return name ? name : "";
}

}  // namespace cups

/// UI text for an IPP job state (not a value written anywhere).
std::string jobState(int state) {
    switch (static_cast<cups::JobState>(state)) {
        case cups::JobState::Pending: return "Waiting";
        case cups::JobState::Held: return "Held";
        case cups::JobState::Processing: return "Printing";
        case cups::JobState::Stopped: return "Stopped";
        case cups::JobState::Canceled: return "Cancelled";
        case cups::JobState::Aborted: return "Aborted";
        case cups::JobState::Completed: return "Completed";
    }
    return "Unknown";
}

std::string printerStatus(const cups::Printer& p) {
    switch (p.state) {
        case cups::PrinterState::Processing: return p.message.empty() ? "Printing" : p.message;
        case cups::PrinterState::Stopped: return p.message.empty() ? "Stopped" : p.message;
        case cups::PrinterState::Idle: return p.accepting ? "Ready" : "Paused";
    }
    return "Ready";
}

/// "#12 · Office_Laser · Printing": the id and queue are data, the state is the only UI word.
std::string jobSubtitle(const cups::JobInfo& job) {
    return "#" + std::to_string(job.id) + " · " + job.dest + " · " + uiText(jobState(job.state));
}

Element jobRow(const cups::JobInfo& job, std::function<void()> onCancel) {
    auto text = VStack({Label(job.title.empty() ? "Untitled job" : job.title).verbatim().lineLimit(1),
                        Label(jobSubtitle(job)).verbatim().font(TextStyle::Subheadline).secondary()})
                    .spacing(1);
    auto cancel = Button(Icon::Xmark).style(ButtonStyle::Borderless).tooltip("Cancel Job");
    cancel.onClick(std::move(onCancel));
    return FormRow(HStack({ui::glyph(Icon::Document), text, Spacer(), cancel}).spacing(10).align(Alignment::Center));
}

class PrintersPanel {
public:
    explicit PrintersPanel(Element anchor) : m_anchor(std::move(anchor)) {
        m_refresh.onClick([this] { refresh(); });
        m_list.setAccessory(m_refresh);
    }

    [[nodiscard]] bool available() const { return cups::Lib::get() != nullptr; }

    std::vector<Element> sections() {
        return {ui::hero(Icon::Printer, theme().colors.gray, "Printers",
                         "Print queues managed by CUPS and the documents waiting in them."),
                m_list.element(), m_jobs.element()};
    }

    void refresh() {
        // Jobs of every printer in one list: per-queue tabs need a model per queue and are not worth it
        // for the handful of documents that are usually in flight.
        std::vector<Element> jobRows;
        for (const auto& j : cups::jobs({})) {
            jobRows.push_back(jobRow(j, [this, id = j.id, dest = j.dest] {
                cups::cancel(id, dest);
                refresh();
            }));
        }
        m_jobs.setRows(jobRows);

        auto list = cups::printers();
        std::vector<Element> rows;
        for (const auto& p : list) rows.push_back(printerRow(p));
        m_list.setRows(rows);
        m_list.setTitle("Printers");
    }

    /// Opens the details popover of the first printer (screenshots).
    void openFirstDetails() {
        const auto list = cups::printers();
        if (!list.empty()) openDetails(list.front());
    }

private:
    Element printerRow(const cups::Printer& p) {
        Color dot = theme().colors.green;
        if (p.state == cups::PrinterState::Processing)
            dot = theme().colors.blue;
        else if (p.state == cups::PrinterState::Stopped || !p.accepting)
            dot = theme().colors.orange;

        auto name = VStack({Label(p.name).verbatim(),
                            Label(printerStatus(p)).font(TextStyle::Subheadline).secondary().lineLimit(1)})
                        .spacing(1);
        auto row = ui::ClickRow({ui::glyph(Icon::Printer, 18, false), name, Spacer()});
        if (p.isDefault) row.add(Label("Default").font(TextStyle::Subheadline).secondary());
        auto badge =
            Canvas([dot](Painter& canvas, const Rect& r) { canvas.fillCircle(r.center(), r.width * 0.3f, dot); });
        badge.frame(10, 10);
        row.add(badge).chevron();
        row.onClick([this, printer = p] { openDetails(printer); });
        return row;
    }

    void openDetails(const cups::Printer& p) {
        std::vector<Element> rows;
        if (p.makeModel.empty())
            rows.push_back(ui::infoRow("Model", "Unknown printer"));
        else
            rows.push_back(ui::infoRow("Model", p.makeModel));
        rows.push_back(ui::infoRow("Status", printerStatus(p)));

        const bool isDefault = p.name == cups::currentDefault();
        if (isDefault) {
            rows.push_back(ui::infoRow("Default Printer", Label("Current default").secondary()));
        } else {
            auto makeDefault = Button("Make Default");
            makeDefault.onClick([this, name = p.name] {
                cups::makeDefault(name);
                refresh();
            });
            rows.push_back(
                FormRow("Default Printer", makeDefault, "New jobs go here unless another printer is chosen"));
        }

        if (gx::haveProgram("lpadmin")) {
            auto toggleAccepting = Button(p.accepting ? "Pause" : "Resume");
            toggleAccepting.onClick([this, name = p.name, accepting = p.accepting] {
                gx::run({"lpadmin", "-p", name, "-o",
                         std::string("printer-is-accepting-jobs=") + (accepting ? "false" : "true")},
                        [this](const gx::ProcessResult& r) {
                            if (!r.ok()) ui::message(m_anchor, "Couldn't change the printer", ui::trim(r.err));
                            refresh();
                        });
            });
            rows.push_back(FormRow("Accept New Jobs", toggleAccepting, "A paused printer keeps jobs in the queue"));
        }

        auto cancelAll = Button("Cancel All Jobs").style(ButtonStyle::Destructive);
        cancelAll.onClick([this, name = p.name, anchor = ui::Anchor(cancelAll)] {
            ui::confirm(anchor, "Cancel all jobs for “" + name + "”?", "Queued documents are discarded.", "Cancel Jobs",
                        [this, name] {
                            for (const auto& j : cups::jobs(name)) cups::cancel(j.id, name);
                            refresh();
                        });
        });
        rows.push_back(FormRow(HStack({Spacer(), cancelAll})));

        auto queued = cups::jobs(p.name);
        std::vector<Element> jobRows;
        for (const auto& j : queued)
            jobRows.push_back(jobRow(j, [this, id = j.id, name = p.name] {
                cups::cancel(id, name);
                refresh();
            }));
        auto details = VStack({ui::section(p.name, rows),
                               ui::section("Jobs", jobRows, "Waiting and printing documents for this printer.")})
                           .spacing(22);
        Popover(VStack({details}).width(400)).show(ui::Anchor(m_refresh));
    }

    Element m_anchor;
    Button m_refresh = Button("Refresh").controlSize(ControlSize::Small);
    ui::DynamicSection m_list{"Printers", "No printers are configured.",
                              "Printers added with CUPS or the desktop's printer tool appear here."};
    ui::DynamicSection m_jobs{"Print Jobs", "No documents are waiting.",
                              "Jobs appear here while they are queued or printing."};
};

}  // namespace

Element printersPanel(Context& ctx) {
    auto panel = std::make_shared<PrintersPanel>(ctx.anchor);
    if (!panel->available())
        return ui::unavailable(Icon::Printer, "Printing Isn't Available",
                               "Install the CUPS client library (libcups) to manage printers here.");
    panel->refresh();
    if (ctx.demo == "details") ctx.app.after(0.8, [p = panel.get()] { p->openFirstDetails(); });
    return ui::page(panel->sections(), panel);
}

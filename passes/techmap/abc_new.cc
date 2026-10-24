/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2024  Martin Povišer <povik@cutebit.org>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/yosys_common.h"
#include "kernel/utils.h"
#include "kernel/threading.h"

#if defined(YOSYS_ENABLE_THREADS) && !defined(YOSYS_LINK_ABC) && !defined(__wasm) && !defined(_WIN32)
// ABC runs as an external process, so several of them can run at once
#  define ABC_NEW_PARALLEL
#  include <cstdlib>
#  include <sys/wait.h>
#endif

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

#ifdef ABC_NEW_PARALLEL
struct AbcJob {
	std::string tmpdir;
	std::string command;
	// abc9_box_id of every box module when the job's input was written. Mapping a
	// deferred box in the meantime renumbers them (abc9_ops -prep_box), but the
	// mapped netlist refers to the boxes by the old numbers.
	dict<IdString, Const> box_ids;
	bool done = false;
};

// Runs off the main thread, so it must not touch the design or the log
void run_abc_job(const AbcJob &job)
{
	std::string cmd = stringf("%s > %s/abc.out 2>&1", job.command, job.tmpdir);
	int status = std::system(cmd.c_str());
	int ret = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
	std::ofstream ret_file(stringf("%s/abc.ret", job.tmpdir));
	ret_file << ret << "\n";
}
#endif

std::vector<Module*> order_modules(Design *design, std::vector<Module *> modules)
{
	std::set<Module *> modules_set(modules.begin(), modules.end());
	using Order = IdString::compare_ptr_by_name<RTLIL::NamedObject>;
	TopoSort<Module*, Order> sort;

	for (auto m : modules) {
		sort.node(m);

		for (auto cell : m->cells()) {
			Module *submodule = design->module(cell->type);
			if (modules_set.count(submodule))
				sort.edge(submodule, m);
		}
	}
	bool is_sorted = sort.sort();
	log_assert(is_sorted);
	return sort.sorted;
}

struct AbcNewPass : public ScriptPass {
	AbcNewPass() : ScriptPass("abc_new", "(experimental) use ABC for SC technology mapping (new)")
	{
		experimental();
	}

	void on_register() override
	{
		RTLIL::constpad["abc_new.script.speed"] = "+&st; &dch -r;" \
			"&nf; &st; &syn2; &if -g -K 6; &synch2 -r;" \
			"&nf; &st; &syn2; &if -g -K 6; &synch2 -r;" \
			"&nf; &st; &syn2; &if -g -K 6; &synch2 -r;" \
			"&nf; &st; &syn2; &if -g -K 6; &synch2 -r;" \
			"&nf; &st; &syn2; &if -g -K 6; &synch2 -r;" \
			"&nf";
	}

	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    abc_new [options] [selection]\n");
		log("\n");
		log("This command uses the ABC tool [1] to optimize the current design and map it to\n");
		log("the target standard cell library.\n");
		log("\n");
		log("    -run <from_label>:<to_label>\n");
		log("        only run the commands between the labels (see below). an empty\n");
		log("        from label is synonymous to 'begin', and empty to label is\n");
		log("        synonymous to the end of the command list.\n");
		log("\n");
		log("    -exe <command>\n");
		log("    -script <file>\n");
		log("    -D <picoseconds>\n");
		log("    -constr <file>\n");
		log("    -dont_use <cell_name>\n");
		log("    -liberty <file>\n");
		log("    -genlib <file>\n");
		log("        these options are passed on to the 'abc9_exe' command which invokes\n");
		log("        the ABC tool on individual modules of the design. please see\n");
		log("        'help abc9_exe' for more details\n");
		log("\n");
		log("    -j <num>\n");
		log("        run ABC on up to <num> modules at once. a module is mapped once all\n");
		log("        of its selected submodules are, and the results are reintegrated in\n");
		log("        the same order as below, so for any <num> above 1 the output is the\n");
		log("        same and does not depend on which ABC process finishes first. the\n");
		log("        default is the number of available cores (see YOSYS_MAX_THREADS).\n");
		log("        -j 1 maps the modules one at a time as in the script below.\n");
		log("\n");
		log("[1] http://www.eecs.berkeley.edu/~alanmi/abc/\n");
		log("\n");
		help_script();
		log("\n");
	}

	bool cleanup;
	std::string abc_exe_options;
	int num_jobs;

	void execute(std::vector<std::string> args, RTLIL::Design *d) override
	{
		std::string run_from, run_to;
		cleanup = true;
		abc_exe_options.clear();
		num_jobs = INT_MAX;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-exe" || args[argidx] == "-script" ||
					args[argidx] == "-D" ||
					args[argidx] == "-constr" || args[argidx] == "-dont_use" ||
					args[argidx] == "-liberty" || args[argidx] == "-genlib") {
				abc_exe_options += " " + args[argidx] + " " + args[argidx + 1];
				argidx++;
			} else if (args[argidx] == "-run" && argidx + 1 < args.size()) {
				size_t pos = args[++argidx].find(':');
				if (pos == std::string::npos)
					break;
				run_from = args[argidx].substr(0, pos);
				run_to = args[argidx].substr(pos + 1);
			} else if (args[argidx] == "-nocleanup") {
				cleanup = false;
			} else if (args[argidx] == "-j" && argidx + 1 < args.size()) {
				num_jobs = atoi(args[++argidx].c_str());
				if (num_jobs < 1)
					log_cmd_error("Invalid -j value '%s'.\n", args[argidx]);
			} else {
				break;
			}
		}
		extra_args(args, argidx, d);

		// If no script provided, use a default.
		if (abc_exe_options.find("-script") == std::string::npos) {
			d->scratchpad_set_string("abc9.script", RTLIL::constpad["abc_new.script.speed"]);
		}

		log_header(d, "Executing ABC_NEW pass.\n");
		log_push();
		run_script(d, run_from, run_to);
		log_pop();
	}

	void script() override
	{
		if (check_label("check")) {
			run("abc9_ops -check");
		}

		if (check_label("prep_boxes")) {
			if (!help_mode) {
				for (auto mod : active_design->selected_whole_modules_warn()) {
					if (mod->get_bool_attribute(ID::abc9_box)) {
						mod->set_bool_attribute(ID::abc9_box, false);
						mod->set_bool_attribute(ID(abc9_deferred_box), true);
					}
				}
			}

			run("box_derive");
			run("abc9_ops -prep_box");
		}

		if (check_label("map")) {
			std::vector<Module *> selected_modules;

			if (!help_mode) {
				selected_modules = order_modules(active_design,
												 active_design->selected_whole_modules_warn());
				active_design->push_empty_selection();
			}

			run("abc9_ops -replace_zbufs");

			if (help_mode) {
				selected_modules = {nullptr};
				run("foreach module in selection");
			}

#ifdef ABC_NEW_PARALLEL
			int num_workers = help_mode ? 0 : ThreadPool::pool_size(0, num_jobs);
			if (num_workers > 1 && GetSize(selected_modules) > 1) {
				map_parallel(selected_modules, num_workers);
				selected_modules.clear();
			}
#endif

			for (auto mod : selected_modules) {
				std::string tmpdir = "<abc-temp-dir>";
				std::string modname = "<module>";
				std::string exe_options = "[options]";
				if (!help_mode) {
					tmpdir = cleanup ? (get_base_tmpdir() + "/") : "_tmp_";
					tmpdir += proc_program_prefix() + "yosys-abc-XXXXXX";
					tmpdir = make_temp_dir(tmpdir);
					modname = mod->name.str();
					exe_options = abc_exe_options;
					log_header(active_design, "Mapping module '%s'.\n", mod);
					log_push();
					active_design->select(mod);
				}

				std::string script_save;
				if (!help_mode && mod->has_attribute(ID(abc9_script))) {
					script_save = active_design->scratchpad_get_string("abc9.script");
					active_design->scratchpad_set_string("abc9.script",
						mod->get_string_attribute(ID(abc9_script)));
				}

				run(stringf("  abc9_ops -write_box %s/input.box", tmpdir));
				run(stringf("  write_xaiger2 -mapping_prep -map2 %s/input.map2 %s/input.xaig", tmpdir, tmpdir));
				run(stringf("  abc9_exe %s -cwd %s -box %s/input.box", exe_options, tmpdir, tmpdir));
				run(stringf("  read_aiger -xaiger -module_name %s$abc9 %s/output.aig",
							modname, tmpdir));
				run(stringf("  abc_ops_reintegrate -stdcell -map %s/input.map2", tmpdir));
				if (!help_mode && mod->has_attribute(ID(abc9_script))) {
					if (script_save.empty())
						active_design->scratchpad_unset("abc9.script");
					else
						active_design->scratchpad_set_string("abc9.script", script_save);
				}

				if (!help_mode) {
					active_design->selection().selected_modules.clear();
					log_pop();

					if (mod->get_bool_attribute(ID(abc9_deferred_box))) {
						mod->set_bool_attribute(ID(abc9_deferred_box), false);
						mod->set_bool_attribute(ID::abc9_box, true);
						Pass::call_on_module(active_design, mod, "portarcs -draw -write");
						run("abc9_ops -prep_box");
					}
				}
			}

			run("abc9_ops -restore_zbufs");

			if (!help_mode) {
				active_design->pop_selection();
			}
		}
	}

#ifdef ABC_NEW_PARALLEL
	// Same steps as the sequential loop in script(), split so that the abc9_exe
	// step of many modules runs on worker threads. Everything that touches the
	// design runs here on the main thread, in an order that depends only on the
	// module order, never on when an ABC process finishes.
	void map_parallel(const std::vector<Module *> &modules, int num_workers)
	{
		int n = GetSize(modules);
		dict<Module *, int> index;
		for (int i = 0; i < n; i++)
			index[modules[i]] = i;

		// The same edges order_modules() sorted by: modules[i] waits for its
		// selected submodules, which all come before it
		std::vector<std::vector<int>> deps(n);
		for (int i = 0; i < n; i++) {
			pool<int> seen;
			for (auto cell : modules[i]->cells()) {
				Module *submodule = active_design->module(cell->type);
				if (submodule && index.count(submodule) && seen.insert(index.at(submodule)).second)
					deps[i].push_back(index.at(submodule));
			}
		}

		std::vector<AbcJob> jobs(n);
		std::vector<bool> launched(n, false), mapped(n, false);
		Mutex done_mutex;
		CondVar done_cond;
		ConcurrentQueue<int> work_queue;

		log("Running ABC on %d modules with up to %d concurrent processes.\n", n, num_workers);

		ThreadPool workers(num_workers, [&](int) {
			while (std::optional<int> i = work_queue.pop_front()) {
				run_abc_job(jobs[*i]);
				{
					LockGuard lock(done_mutex);
					jobs[*i].done = true;
				}
				done_cond.notify_all();
			}
		});
		// Declared after `workers` so the queue is closed, and the workers can
		// exit, before they are joined, also when an error unwinds this function
		struct QueueCloser {
			ConcurrentQueue<int> &queue;
			~QueueCloser() { queue.close(); }
		} closer{work_queue};

		for (int next = 0; next < n; next++) {
			for (int i = next; i < n; i++) {
				if (launched[i])
					continue;
				bool ready = true;
				for (int dep : deps[i])
					ready = ready && mapped[dep];
				if (!ready)
					continue;
				prepare_module(modules[i], jobs[i]);
				launched[i] = true;
				work_queue.push_back(i);
			}

			log_assert(launched[next]);
			{
				UniqueLock lock(done_mutex);
				done_cond.wait(lock, [&] { return jobs[next].done; });
			}
			collect_module(modules[next], jobs[next]);
			mapped[next] = true;
		}

		// portarcs leaves the $input_port cells of buffer-normalized mode in the
		// design. The sequential loop mostly hides that because the next
		// write_xaiger2 removes them, but here many modules are mapped after the
		// last write_xaiger2.
		for (auto module : active_design->modules()) {
			std::vector<Cell *> to_remove;
			for (auto cell : module->cells())
				if (cell->type == ID($input_port))
					to_remove.push_back(cell);
			for (auto cell : to_remove)
				module->remove(cell);
		}
	}

	void prepare_module(Module *mod, AbcJob &job)
	{
		job.tmpdir = cleanup ? (get_base_tmpdir() + "/") : "_tmp_";
		job.tmpdir += proc_program_prefix() + "yosys-abc-XXXXXX";
		job.tmpdir = make_temp_dir(job.tmpdir);
		log_header(active_design, "Preparing module '%s' for ABC.\n", mod);
		log_push();
		active_design->select(mod);

		std::string script_save;
		if (mod->has_attribute(ID(abc9_script))) {
			script_save = active_design->scratchpad_get_string("abc9.script");
			active_design->scratchpad_set_string("abc9.script",
				mod->get_string_attribute(ID(abc9_script)));
		}

		run(stringf("  abc9_ops -write_box %s/input.box", job.tmpdir));
		run(stringf("  write_xaiger2 -mapping_prep -map2 %s/input.map2 %s/input.xaig", job.tmpdir, job.tmpdir));
		run(stringf("  abc9_exe %s -prepare -cwd %s -box %s/input.box", abc_exe_options, job.tmpdir, job.tmpdir));
		job.command = active_design->scratchpad_get_string("abc9_exe.command");
		active_design->scratchpad_unset("abc9_exe.command");
		for (auto box : active_design->modules())
			if (box->has_attribute(ID::abc9_box_id))
				job.box_ids[box->name] = box->attributes.at(ID::abc9_box_id);

		if (mod->has_attribute(ID(abc9_script))) {
			if (script_save.empty())
				active_design->scratchpad_unset("abc9.script");
			else
				active_design->scratchpad_set_string("abc9.script", script_save);
		}

		active_design->selection().selected_modules.clear();
		log_pop();
	}

	void collect_module(Module *mod, const AbcJob &job)
	{
		log_header(active_design, "Mapping module '%s'.\n", mod);
		log_push();
		active_design->select(mod);

		run(stringf("  abc9_exe %s -collect -cwd %s -box %s/input.box", abc_exe_options, job.tmpdir, job.tmpdir));

		dict<IdString, Const> current_box_ids;
		for (auto &it : job.box_ids) {
			Module *box = active_design->module(it.first);
			if (!box || !box->has_attribute(ID::abc9_box_id))
				continue;
			current_box_ids[it.first] = box->attributes.at(ID::abc9_box_id);
			box->attributes[ID::abc9_box_id] = it.second;
		}
		run(stringf("  read_aiger -xaiger -module_name %s$abc9 %s/output.aig",
					mod->name.str(), job.tmpdir));
		run(stringf("  abc_ops_reintegrate -stdcell -map %s/input.map2", job.tmpdir));
		for (auto &it : current_box_ids)
			active_design->module(it.first)->attributes[ID::abc9_box_id] = it.second;

		active_design->selection().selected_modules.clear();
		log_pop();

		if (mod->get_bool_attribute(ID(abc9_deferred_box))) {
			mod->set_bool_attribute(ID(abc9_deferred_box), false);
			mod->set_bool_attribute(ID::abc9_box, true);
			Pass::call_on_module(active_design, mod, "portarcs -draw -write");
			run("abc9_ops -prep_box");
		}
	}
#endif
} AbcNewPass;

PRIVATE_NAMESPACE_END

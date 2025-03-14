// Copyright (c) 2022, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <array>
#include <random>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <solv/solver.h>

#include "mamba/core/channel.hpp"
#include "mamba/core/mamba_fs.hpp"
#include "mamba/core/package_info.hpp"
#include "mamba/core/pool.hpp"
#include "mamba/core/prefix_data.hpp"
#include "mamba/core/repo.hpp"
#include "mamba/core/satisfiability_error.hpp"
#include "mamba/core/solver.hpp"
#include "mamba/core/subdirdata.hpp"
#include "mamba/core/util.hpp"
#include "mamba/core/util_random.hpp"
#include "mamba/core/util_string.hpp"

namespace mamba
{
    TEST_SUITE("conflict_map")
    {
        TEST_CASE("symetric")
        {
            auto c = conflict_map<std::size_t>();
            CHECK_EQ(c.size(), 0);
            CHECK_FALSE(c.has_conflict(0));
            CHECK_FALSE(c.in_conflict(0, 1));
            CHECK(c.add(0, 1));
            CHECK(c.add(1, 2));
            CHECK_FALSE(c.add(1, 2));
            CHECK(c.has_conflict(0));
            CHECK(c.in_conflict(0, 1));
            CHECK(c.in_conflict(1, 2));
            CHECK(c.has_conflict(2));
            CHECK_FALSE(c.in_conflict(0, 2));
            // With same
            CHECK(c.add(5, 5));
            CHECK(c.has_conflict(5));
            CHECK(c.in_conflict(5, 5));
        }

        TEST_CASE("remove")
        {
            auto c = conflict_map<std::size_t>({ { 1, 1 }, { 1, 2 }, { 1, 3 }, { 2, 4 } });
            REQUIRE_EQ(c.size(), 4);

            REQUIRE(c.in_conflict(2, 4));
            REQUIRE(c.in_conflict(4, 2));
            CHECK(c.remove(2, 4));
            CHECK_FALSE(c.in_conflict(4, 2));
            CHECK_FALSE(c.in_conflict(2, 4));
            CHECK(c.has_conflict(2));
            CHECK_FALSE(c.has_conflict(4));

            CHECK_FALSE(c.remove(2, 4));

            CHECK(c.remove(1));
            CHECK_FALSE(c.has_conflict(1));
            CHECK_FALSE(c.in_conflict(1, 1));
            CHECK_FALSE(c.in_conflict(1, 2));
            CHECK_FALSE(c.in_conflict(3, 1));
        }
    }

    /**
     * A RAII object to ensure a path exists only for the lifetime of the guard.
     */
    struct dir_guard
    {
        fs::u8path path;

        dir_guard(fs::u8path&& path_)
            : path(std::move(path_))
        {
            fs::create_directories(path);
        }
        ~dir_guard()
        {
            fs::remove_all(path);
        }
    };

    /**
     * Simple factory for building a PackageInfo.
     */
    auto mkpkg(std::string name, std::string version, std::vector<std::string> dependencies = {})
        -> PackageInfo
    {
        auto pkg = PackageInfo(std::move(name));
        pkg.version = std::move(version);
        pkg.depends = std::move(dependencies);
        pkg.build_string = "bld";
        return pkg;
    }


    /**
     * Create the repodata.json file containing the package information.
     */
    template <typename PkgRange>
    auto create_repodata_json(fs::u8path dir, const PkgRange& packages) -> fs::u8path
    {
        namespace nl = nlohmann;

        auto packages_j = nl::json::object();
        for (const auto& pkg : packages)
        {
            auto fname = fmt::format("{}-{}-{}.tar.bz2", pkg.name, pkg.version, pkg.build_string);
            packages_j[std::move(fname)] = pkg.json_record();
        }
        auto repodata_j = nl::json::object();
        repodata_j["packages"] = std::move(packages_j);

        fs::create_directories(dir / "noarch");
        auto repodata_f = dir / "noarch/repodata.json";
        open_ofstream(repodata_f, std::ofstream::app) << repodata_j;

        return repodata_f;
    }

    /**
     * Create a solver and a pool of a conflict.
     *
     * The underlying packages do not exist, we are onl interested in the conflict.
     */
    template <typename PkgRange>
    auto create_problem(const PkgRange& packages, const std::vector<std::string>& specs)
    {
        const auto tmp_dir = dir_guard(
            fs::temp_directory_path() / "mamba/tests" / generate_random_alphanumeric_string(20)
        );
        const auto repodata_f = create_repodata_json(tmp_dir.path, packages);

        auto pool = MPool();
        MRepo(pool, "some-name", repodata_f, RepoMetadata{ /* .url= */ "some-url" });
        auto solver = std::make_unique<MSolver>(
            std::move(pool),
            std::vector{ std::pair{ SOLVER_FLAG_ALLOW_DOWNGRADE, 1 } }
        );
        solver->add_jobs(specs, SOLVER_INSTALL);

        return solver;
    }

    /**
     * Test the test utility function.
     */
    TEST_SUITE("satifiability_error")
    {
        TEST_CASE("create_problem")
        {
            auto solver = create_problem(std::array{ mkpkg("foo", "0.1.0", {}) }, { "foo" });
            const auto solved = solver->try_solve();
            REQUIRE(solved);
        }
    }

    auto create_basic_conflict() -> MSolver&
    {
        static auto solver = create_problem(
            std::array{
                mkpkg("A", "0.1.0"),
                mkpkg("A", "0.2.0"),
                mkpkg("A", "0.3.0"),
            },
            { "A=0.4.0" }
        );
        return *solver;
    }

    /**
     * Create the PubGrub blog post example.
     *
     * The example given by Natalie Weizenbaum
     * (credits https://nex3.medium.com/pubgrub-2fb6470504f).
     */
    auto create_pubgrub() -> MSolver&
    {
        static auto solver = create_problem(
            std::array{
                mkpkg("menu", "1.5.0", { "dropdown=2.*" }),
                mkpkg("menu", "1.4.0", { "dropdown=2.*" }),
                mkpkg("menu", "1.3.0", { "dropdown=2.*" }),
                mkpkg("menu", "1.2.0", { "dropdown=2.*" }),
                mkpkg("menu", "1.1.0", { "dropdown=2.*" }),
                mkpkg("menu", "1.0.0", { "dropdown=1.*" }),
                mkpkg("dropdown", "2.3.0", { "icons=2.*" }),
                mkpkg("dropdown", "2.2.0", { "icons=2.*" }),
                mkpkg("dropdown", "2.1.0", { "icons=2.*" }),
                mkpkg("dropdown", "2.0.0", { "icons=2.*" }),
                mkpkg("dropdown", "1.8.0", { "icons=1.*", "intl=3.*" }),
                mkpkg("icons", "2.0.0"),
                mkpkg("icons", "1.0.0"),
                mkpkg("intl", "5.0.0"),
                mkpkg("intl", "4.0.0"),
                mkpkg("intl", "3.0.0"),
            },
            { "menu", "icons=1.*", "intl=5.*" }
        );
        return *solver;
    }

    auto create_pubgrub_hard_(bool missing_package)
    {
        auto packages = std::vector{
            mkpkg("menu", "2.1.0", { "dropdown>=2.1", "emoji" }),
            mkpkg("menu", "2.0.1", { "dropdown>=2", "emoji" }),
            mkpkg("menu", "2.0.0", { "dropdown>=2", "emoji" }),
            mkpkg("menu", "1.5.0", { "dropdown=2.*", "emoji" }),
            mkpkg("menu", "1.4.0", { "dropdown=2.*", "emoji" }),
            mkpkg("menu", "1.3.0", { "dropdown=2.*" }),
            mkpkg("menu", "1.2.0", { "dropdown=2.*" }),
            mkpkg("menu", "1.1.0", { "dropdown=1.*" }),
            mkpkg("menu", "1.0.0", { "dropdown=1.*" }),
            mkpkg("emoji", "1.1.0", { "libicons=2.*" }),
            mkpkg("emoji", "1.0.0", { "libicons=2.*" }),
            mkpkg("dropdown", "2.3.0", { "libicons=2.*" }),
            mkpkg("dropdown", "2.2.0", { "libicons=2.*" }),
            mkpkg("dropdown", "2.1.0", { "libicons=2.*" }),
            mkpkg("dropdown", "2.0.0", { "libicons=2.*" }),
            mkpkg("dropdown", "1.8.0", { "libicons=1.*", "intl=3.*" }),
            mkpkg("dropdown", "1.7.0", { "libicons=1.*", "intl=3.*" }),
            mkpkg("dropdown", "1.6.0", { "libicons=1.*", "intl=3.*" }),
            mkpkg("pyicons", "2.0.0", { "libicons=2.*" }),
            mkpkg("pyicons", "1.1.0", { "libicons=1.2.*" }),
            mkpkg("pyicons", "1.0.0", { "libicons=1.*" }),
            mkpkg("pretty", "1.1.0", { "pyicons=1.1.*" }),
            mkpkg("pretty", "1.0.1", { "pyicons=1.*" }),
            mkpkg("pretty", "1.0.0", { "pyicons=1.*" }),
            mkpkg("intl", "5.0.0"),
            mkpkg("intl", "4.0.0"),
            mkpkg("intl", "3.2.0"),
            mkpkg("intl", "3.1.0"),
            mkpkg("intl", "3.0.0"),
            mkpkg("intl-mod", "1.0.0", { "intl=5.0.*" }),
            mkpkg("intl-mod", "1.0.1", { "intl=5.0.*" }),
            mkpkg("libicons", "2.1.0"),
            mkpkg("libicons", "2.0.1"),
            mkpkg("libicons", "2.0.0"),
            mkpkg("libicons", "1.2.1"),
            mkpkg("libicons", "1.2.0"),
            mkpkg("libicons", "1.0.0"),
        };

        if (missing_package)
        {
            packages.push_back(mkpkg("dropdown", "2.9.3", { "libnothere>1.0" }));
            packages.push_back(mkpkg("dropdown", "2.9.2", { "libicons>10.0", "libnothere>1.0" }));
            packages.push_back(mkpkg("dropdown", "2.9.1", { "libicons>10.0", "libnothere>1.0" }));
            packages.push_back(mkpkg("dropdown", "2.9.0", { "libicons>10.0" }));
        }
        return create_problem(
            packages,
            { "menu", "pyicons=1.*", "intl=5.*", "intl-mod", "pretty>=1.0" }
        );
    }

    /**
     * A harder version of ``create_pubgrub``.
     */
    auto create_pubgrub_hard() -> MSolver&
    {
        static auto solver = create_pubgrub_hard_(false);
        return *solver;
    }

    /**
     * The hard version of the alternate PubGrub with missing packages.
     */
    auto create_pubgrub_missing() -> MSolver&
    {
        static auto solver = create_pubgrub_hard_(true);
        return *solver;
    }

    template <typename T, typename E>
    auto expected_value_or_throw(tl::expected<T, E>&& ex)
    {
        if (!ex.has_value())
        {
            throw ex.error();
        }
        return std::move(ex).value();
    }

    auto
    make_platform_channels(std::vector<std::string>&& channels, const std::vector<std::string>& platforms)
        -> std::vector<std::string>
    {
        auto add_plat = [&platforms](const auto& chan)
        { return fmt::format("{}[{}]", chan, fmt::join(platforms, ",")); };
        std::transform(channels.begin(), channels.end(), channels.begin(), add_plat);
        return std::move(channels);
    }

    /**
     * Mock of channel_loader.hpp:load_channels that takes a list of channels.
     */
    auto load_channels(MPool& pool, MultiPackageCache& cache, std::vector<std::string>&& channels)
    {
        auto dlist = MultiDownloadTarget();
        auto sub_dirs = std::vector<MSubdirData>();
        for (const auto* chan : get_channels(channels))
        {
            for (auto& [platform, url] : chan->platform_urls(true))
            {
                auto sub_dir = expected_value_or_throw(MSubdirData::create(*chan, platform, url, cache)
                );
                dlist.add(sub_dir.target());
                sub_dirs.push_back(std::move(sub_dir));
            }
        }
        dlist.download(MAMBA_DOWNLOAD_FAILFAST);
        for (auto& sub_dir : sub_dirs)
        {
            sub_dir.create_repo(pool);
        }
    }

    /**
     * Create a solver and a pool of a conflict from conda-forge packages.
     */
    auto create_conda_forge(
        std::vector<std::string>&& specs,
        const std::vector<PackageInfo>& virtual_packages = { mkpkg("__glibc", "2.17.0") },
        std::vector<std::string>&& channels = { "conda-forge" },
        const std::vector<std::string>& platforms = { "linux-64", "noarch" }
    )
    {
        // Reusing the cache for all invocation of this funciton for speedup
        static const auto tmp_dir = dir_guard(
            fs::temp_directory_path() / "mamba/tests" / generate_random_alphanumeric_string(20)
        );

        auto prefix_data = expected_value_or_throw(PrefixData::create(tmp_dir.path / "prefix"));
        prefix_data.add_packages(virtual_packages);
        auto pool = MPool();
        auto repo = MRepo(pool, prefix_data);
        repo.set_installed();

        auto cache = MultiPackageCache({ tmp_dir.path / "cache" });
        create_cache_dir(cache.first_writable_path());

        bool prev_progress_bars_value = Context::instance().graphics_params.no_progress_bars;
        Context::instance().graphics_params.no_progress_bars = true;
        load_channels(pool, cache, make_platform_channels(std::move(channels), platforms));
        Context::instance().graphics_params.no_progress_bars = prev_progress_bars_value;

        auto solver = std::make_unique<MSolver>(
            std::move(pool),
            std::vector{ std::pair{ SOLVER_FLAG_ALLOW_DOWNGRADE, 1 } }
        );
        solver->add_jobs(specs, SOLVER_INSTALL);

        return solver;
    }

    /**
     * Test the test utility function.
     */
    TEST_SUITE("satifiability_error")
    {
        TEST_CASE("create_conda_forge")
        {
            auto solver = create_conda_forge({ "xtensor>=0.7" });
            const auto solved = solver->try_solve();
            REQUIRE(solved);
        }
    }

    auto create_pytorch_cpu() -> MSolver&
    {
        static auto solver = create_conda_forge({ "python=2.7", "pytorch=1.12" });
        return *solver;
    }

    auto create_pytorch_cuda() -> MSolver&
    {
        static auto solver = create_conda_forge(
            { "python=2.7", "pytorch=1.12" },
            { mkpkg("__glibc", "2.17.0"), mkpkg("__cuda", "10.2.0") }
        );
        return *solver;
    }

    auto create_cudatoolkit() -> MSolver&
    {
        static auto solver = create_conda_forge(
            { "python=3.7", "cudatoolkit=11.1", "cudnn=8.0", "pytorch=1.8", "torchvision=0.9=*py37_cu111*" },
            { mkpkg("__glibc", "2.17.0"), mkpkg("__cuda", "11.1") }
        );
        return *solver;
    }

    auto create_jpeg9b() -> MSolver&
    {
        static auto solver = create_conda_forge({ "python=3.7", "jpeg=9b" });
        return *solver;
    }

    auto create_r_base() -> MSolver&
    {
        static auto solver = create_conda_forge(
            { "r-base=3.5.* ", "pandas=0", "numpy<1.20.0", "matplotlib=2", "r-matchit=4.*" }
        );
        return *solver;
    }

    auto create_scip() -> MSolver&
    {
        static auto solver = create_conda_forge({ "scip=8.*", "pyscipopt<4.0" });
        return *solver;
    }

    auto create_jupyterlab() -> MSolver&
    {
        static auto solver = create_conda_forge({ "jupyterlab=3.4", "openssl=3.0.0" });
        return *solver;
    }

    auto create_double_python() -> MSolver&
    {
        static auto solver = create_conda_forge({ "python=3.9.*", "python=3.10.*" });
        return *solver;
    }

    auto create_numba() -> MSolver&
    {
        static auto solver = create_conda_forge({ "python=3.11", "numba<0.56" });
        return *solver;
    }

    template <typename NodeVariant>
    auto is_virtual_package(const NodeVariant& node) -> bool
    {
        return std::visit(
            [](const auto& n) -> bool
            {
                using Node = std::remove_const_t<std::remove_reference_t<decltype(n)>>;
                if constexpr (!std::is_same_v<Node, ProblemsGraph::RootNode>)
                {
                    return starts_with(std::invoke(&Node::name, n), "__");
                }
                return false;
            },
            node
        );
    };

    namespace
    {
        std::vector<decltype(&create_basic_conflict)> pb_values = {
            create_basic_conflict, create_pubgrub,      create_pubgrub_hard, create_pubgrub_missing,
            create_pytorch_cpu,    create_pytorch_cuda, create_cudatoolkit,  create_jpeg9b,
            create_r_base,         create_scip,         create_jupyterlab,   create_double_python,
            create_numba
        };
    }

    TEST_SUITE("satifiability_error")
    {
        TEST_CASE("NamedList")
        {
            auto l = CompressedProblemsGraph::PackageListNode();
            static constexpr std::size_t n_packages = 9;
            for (std::size_t minor = 1; minor <= n_packages; ++minor)
            {
                l.insert({ mkpkg("pkg", fmt::format("0.{}.0", minor)) });
            }
            CHECK_EQ(l.size(), n_packages);
            CHECK_EQ(l.name(), "pkg");
            {
                auto [str, size] = l.versions_trunc(", ", "...", 5);
                CHECK_EQ(size, 9);
                CHECK_EQ(str, "0.1.0, 0.2.0, ..., 0.9.0");
            }
            {
                auto [str, size] = l.build_strings_trunc(", ", "...", 5, false);
                CHECK_EQ(size, 9);
                CHECK_EQ(str, "bld, bld, ..., bld");
            }
            {
                auto [str, size] = l.build_strings_trunc(", ", "...", 5, true);
                CHECK_EQ(size, 1);
                CHECK_EQ(str, "bld");
            }
            {
                auto [str, size] = l.versions_and_build_strings_trunc("|", "---", 5);
                CHECK_EQ(size, 9);
                CHECK_EQ(str, "0.1.0 bld|0.2.0 bld|---|0.9.0 bld");
            }
        }

        TEST_CASE("constructor")
        {
            for (auto& p : pb_values)
            {
                CAPTURE(p);
                auto& solver = p();
                const auto solved = solver.try_solve();
                REQUIRE_FALSE(solved);
                const auto pbs = ProblemsGraph::from_solver(solver, solver.pool());
                const auto& g = pbs.graph();

                REQUIRE_GE(g.number_of_nodes(), 1);
                g.for_each_node_id(
                    [&](auto id)
                    {
                        const auto& node = g.node(id);
                        // Currently we do not make assumption about virtual package since
                        // we are not sure we are including them the same way than they would be in
                        // practice
                        if (!is_virtual_package(node))
                        {
                            if (g.in_degree(id) == 0)
                            {
                                // Only one root node
                                CHECK_EQ(id, pbs.root_node());
                                CHECK(std::holds_alternative<ProblemsGraph::RootNode>(node));
                            }
                            else if (g.out_degree(id) == 0)
                            {
                                CHECK_FALSE(std::holds_alternative<ProblemsGraph::RootNode>(node));
                            }
                            else
                            {
                                CHECK(std::holds_alternative<ProblemsGraph::PackageNode>(node));
                            }
                            // All nodes reachable from the root
                            CHECK(is_reachable(pbs.graph(), pbs.root_node(), id));
                        }
                    }
                );

                const auto& conflicts = pbs.conflicts();
                for (const auto& [n, _] : conflicts)
                {
                    bool tmp = std::holds_alternative<ProblemsGraph::PackageNode>(g.node(n))
                               || std::holds_alternative<ProblemsGraph::ConstraintNode>(g.node(n));
                    CHECK(tmp);
                }
            }
        }

        TEST_CASE("simplify_conflicts")
        {
            for (auto& p : pb_values)
            {
                CAPTURE(p);
                auto& solver = p();
                const auto solved = solver.try_solve();
                REQUIRE_FALSE(solved);
                const auto& pbs = ProblemsGraph::from_solver(solver, solver.pool());
                const auto& pbs_simplified = simplify_conflicts(pbs);
                const auto& graph_simplified = pbs_simplified.graph();

                REQUIRE_GE(graph_simplified.number_of_nodes(), 1);
                REQUIRE_LE(graph_simplified.number_of_nodes(), pbs.graph().number_of_nodes());

                for (const auto& [id, _] : pbs_simplified.conflicts())
                {
                    const auto& node = graph_simplified.node(id);
                    // Currently we do not make assumption about virtual package since
                    // we are not sure we are including them the same way than they would be in
                    // practice
                    if (!is_virtual_package(node))
                    {
                        CHECK(graph_simplified.has_node(id));
                        // Unfortunately not all conflicts are on leaves
                        // CHECK_EQ(graph_simplified.out_degree(id), 0);
                        CHECK(is_reachable(graph_simplified, pbs_simplified.root_node(), id));
                    }
                }
            }
        }

        TEST_CASE("compression")
        {
            using CpPbGr = CompressedProblemsGraph;

            for (auto& p : pb_values)
            {
                CAPTURE(p);
                auto& solver = p();
                const auto solved = solver.try_solve();
                REQUIRE_FALSE(solved);
                const auto pbs = ProblemsGraph::from_solver(solver, solver.pool());
                const auto cp_pbs = CpPbGr::from_problems_graph(simplify_conflicts(pbs));
                const auto& cp_g = cp_pbs.graph();

                REQUIRE_GE(pbs.graph().number_of_nodes(), cp_g.number_of_nodes());
                REQUIRE_GE(cp_g.number_of_nodes(), 1);
                cp_g.for_each_node_id(
                    [&](auto id)
                    {
                        const auto& node = cp_g.node(id);
                        // Currently we do not make assumption about virtual package since
                        // we are not sure we are including them the same way than they would be in
                        if (!is_virtual_package(node))
                        {
                            if (cp_g.in_degree(id) == 0)
                            {
                                // Only one root node
                                CHECK_EQ(id, pbs.root_node());
                                CHECK(std::holds_alternative<CpPbGr::RootNode>(node));
                            }
                            else if (cp_g.out_degree(id) == 0)
                            {
                                CHECK_FALSE(std::holds_alternative<CpPbGr::RootNode>(node));
                            }
                            else
                            {
                                CHECK(std::holds_alternative<CpPbGr::PackageListNode>(node));
                            }
                            // All nodes reachable from the root
                            CHECK(is_reachable(cp_g, cp_pbs.root_node(), id));
                        }
                    }
                );

                const auto& conflicts = cp_pbs.conflicts();
                for (const auto& [n, _] : conflicts)
                {
                    bool tmp = std::holds_alternative<CpPbGr::PackageListNode>(cp_g.node(n))
                               || std::holds_alternative<CpPbGr::ConstraintListNode>(cp_g.node(n));
                    CHECK(tmp);
                }
            }
        }

        TEST_CASE("problem_tree_str")
        {
            using CpPbGr = CompressedProblemsGraph;

            for (auto& p : pb_values)
            {
                CAPTURE(p);
                auto& solver = p();
                const auto solved = solver.try_solve();
                REQUIRE_FALSE(solved);
                const auto pbs = ProblemsGraph::from_solver(solver, solver.pool());
                const auto cp_pbs = CpPbGr::from_problems_graph(simplify_conflicts(pbs));
                const auto message = problem_tree_msg(cp_pbs);

                auto message_contains = [&message](const auto& node)
                {
                    using Node = std::remove_cv_t<std::remove_reference_t<decltype(node)>>;
                    if constexpr (!std::is_same_v<Node, CpPbGr::RootNode>)
                    {
                        CHECK(contains(message, node.name()));
                    }
                };

                cp_pbs.graph().for_each_node_id(
                    [&message_contains, &g = cp_pbs.graph()](auto id)
                    {
                        std::visit(message_contains, g.node(id));  //
                    }
                );
            }
        }
    }
}

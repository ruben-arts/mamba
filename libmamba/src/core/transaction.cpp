// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <iostream>
#include <stack>

#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <solv/selection.h>
extern "C"  // Incomplete header
{
#include <solv/conda.h>
}

#include "mamba/core/channel.hpp"
#include "mamba/core/context.hpp"
#include "mamba/core/execution.hpp"
#include "mamba/core/link.hpp"
#include "mamba/core/match_spec.hpp"
#include "mamba/core/output.hpp"
#include "mamba/core/pool.hpp"
#include "mamba/core/thread_utils.hpp"
#include "mamba/core/transaction.hpp"
#include "mamba/core/util_scope.hpp"
#include "mamba/core/util_string.hpp"
#include "solv-cpp/queue.hpp"

#include "progress_bar_impl.hpp"

namespace mamba
{
    namespace
    {
        bool need_pkg_download(const PackageInfo& pkg_info, MultiPackageCache& caches)
        {
            return caches.get_extracted_dir_path(pkg_info).empty()
                   && caches.get_tarball_path(pkg_info).empty();
        }

        auto mk_pkginfo(const MPool& pool, Solvable* s) -> PackageInfo
        {
            assert(s != nullptr);
            const auto pkginfo = pool.id2pkginfo(pool_solvable2id(pool, s));
            assert(pkginfo.has_value());  // There is Solvable so the optional must no be empty
            return std::move(pkginfo).value();
        };

    }

    nlohmann::json solvable_to_json(const MPool& pool, Solvable* s)
    {
        return mk_pkginfo(pool, s).json_record();
    }

    /********************************
     * PackageDownloadExtractTarget *
     ********************************/

    counting_semaphore DownloadExtractSemaphore::semaphore(0);

    std::ptrdiff_t DownloadExtractSemaphore::get_max()
    {
        return DownloadExtractSemaphore::semaphore.get_max();
    }

    void DownloadExtractSemaphore::set_max(int value)
    {
        DownloadExtractSemaphore::semaphore.set_max(value);
    }

    PackageDownloadExtractTarget::PackageDownloadExtractTarget(const PackageInfo& pkg_info)
        : m_finished(false)
        , m_package_info(pkg_info)
    {
        m_filename = pkg_info.fn;

        // only do this for micromamba for now
        if (Context::instance().command_params.is_micromamba)
        {
            m_url = make_channel(pkg_info.url).urls(true)[0];
        }
        else
        {
            m_url = pkg_info.url;
        }

        m_name = pkg_info.name;

        m_expected_size = pkg_info.size;
        m_sha256 = pkg_info.sha256;
        m_md5 = pkg_info.md5;

        auto& ctx = Context::instance();
        m_has_progress_bars = !(
            ctx.graphics_params.no_progress_bars || ctx.output_params.quiet || ctx.output_params.json
        );
    }

    void PackageDownloadExtractTarget::write_repodata_record(const fs::u8path& base_path)
    {
        fs::u8path repodata_record_path = base_path / "info" / "repodata_record.json";
        fs::u8path index_path = base_path / "info" / "index.json";

        nlohmann::json index, solvable_json;
        std::ifstream index_file = open_ifstream(index_path);
        index_file >> index;

        solvable_json = m_package_info.json_record();
        index.insert(solvable_json.cbegin(), solvable_json.cend());

        if (index.find("size") == index.end() || index["size"] == 0)
        {
            index["size"] = fs::file_size(m_tarball_path);
        }

        std::ofstream repodata_record(repodata_record_path.std_path());
        repodata_record << index.dump(4);
    }

    static std::mutex urls_txt_mutex;
    void PackageDownloadExtractTarget::add_url()
    {
        std::lock_guard<std::mutex> lock(urls_txt_mutex);
        const auto urls_file_path = m_cache_path / "urls.txt";
        std::ofstream urls_txt(urls_file_path.std_path(), std::ios::app);
        urls_txt << m_url << std::endl;
    }

    void PackageDownloadExtractTarget::validate()
    {
        m_validation_result = VALIDATION_RESULT::VALID;
        if (m_expected_size && (m_target->get_downloaded_size() != m_expected_size))
        {
            LOG_ERROR << "File not valid: file size doesn't match expectation " << m_tarball_path
                      << "\nExpected: " << m_expected_size
                      << "\nActual: " << m_target->get_downloaded_size() << "\n";
            if (m_has_progress_bars)
            {
                m_download_bar.set_postfix("validation failed");
                m_download_bar.mark_as_completed();
            }
            Console::instance().print(m_filename + " tarball has incorrect size");
            m_validation_result = SIZE_ERROR;
            return;
        }
        interruption_point();

        if (!m_sha256.empty())
        {
            auto sha256sum = validation::sha256sum(m_tarball_path);
            if (m_sha256 != sha256sum)
            {
                m_validation_result = SHA256_ERROR;
                if (m_has_progress_bars)
                {
                    m_download_bar.set_postfix("validation failed");
                    m_download_bar.mark_as_completed();
                }
                Console::instance().print(m_filename + " tarball has incorrect checksum");
                LOG_ERROR << "File not valid: SHA256 sum doesn't match expectation " << m_tarball_path
                          << "\nExpected: " << m_sha256 << "\nActual: " << sha256sum << "\n";
            }
            return;
        }
        if (!m_md5.empty())
        {
            auto md5sum = validation::md5sum(m_tarball_path);
            if (m_md5 != md5sum)
            {
                m_validation_result = MD5SUM_ERROR;
                if (m_has_progress_bars)
                {
                    m_download_bar.set_postfix("validation failed");
                    m_download_bar.mark_as_completed();
                }
                Console::instance().print(m_filename + " tarball has incorrect checksum");
                LOG_ERROR << "File not valid: MD5 sum doesn't match expectation " << m_tarball_path
                          << "\nExpected: " << m_md5 << "\nActual: " << md5sum << "\n";
            }
        }
    }

    std::function<void(ProgressBarRepr&)> PackageDownloadExtractTarget::extract_repr()
    {
        return [&](ProgressBarRepr& r) -> void
        {
            if (r.progress_bar().started())
            {
                r.postfix.set_value("Extracting");
            }
            else
            {
                r.postfix.set_value("Extracted");
            }
        };
    }

    std::function<void(ProgressProxy&)> PackageDownloadExtractTarget::extract_progress_callback()
    {
        return [&](ProgressProxy& bar) -> void
        {
            if (bar.started())
            {
                bar.set_progress(0, 1);
            }
        };
    }

    bool PackageDownloadExtractTarget::extract()
    {
        // Extracting is __not__ yet thread safe it seems...
        interruption_point();

        if (m_has_progress_bars)
        {
            m_extract_bar.start();
        }

        LOG_DEBUG << "Waiting for decompression " << m_tarball_path;
        if (m_has_progress_bars)
        {
            m_extract_bar.update_progress(0, 1);
        }
        {
            std::lock_guard<counting_semaphore> lock(DownloadExtractSemaphore::semaphore);
            interruption_point();
            LOG_DEBUG << "Decompressing '" << m_tarball_path.string() << "'";
            fs::u8path extract_path;
            try
            {
                std::string fn = m_filename;
                if (ends_with(fn, ".tar.bz2"))
                {
                    fn = fn.substr(0, fn.size() - 8);
                }
                else if (ends_with(fn, ".conda"))
                {
                    fn = fn.substr(0, fn.size() - 6);
                }
                else
                {
                    LOG_ERROR << "Unknown package format '" << m_filename << "'";
                    throw std::runtime_error("Unknown package format.");
                }
                // Be sure the first writable cache doesn't contain invalid extracted package
                extract_path = m_cache_path / fn;
                if (fs::exists(extract_path))
                {
                    LOG_DEBUG << "Removing '" << extract_path.string()
                              << "' before extracting it again";
                    fs::remove_all(extract_path);
                }

                // Use non-subproc version if concurrency is disabled to avoid
                // any potential subprocess issues
                if (DownloadExtractSemaphore::get_max() == 1)
                {
                    mamba::extract(m_tarball_path, extract_path);
                }
                else
                {
                    mamba::extract_subproc(m_tarball_path, extract_path);
                }
                // mamba::extract(m_tarball_path, extract_path);
                interruption_point();
                LOG_DEBUG << "Extracted to '" << extract_path.string() << "'";
                write_repodata_record(extract_path);
                add_url();

                if (m_has_progress_bars)
                {
                    m_extract_bar.set_full();
                    m_extract_bar.mark_as_completed();
                }
            }
            catch (std::exception& e)
            {
                Console::instance().print(m_filename + " extraction failed");
                LOG_ERROR << "Error when extracting package: " << e.what();
                m_decompress_exception = e;
                m_validation_result = VALIDATION_RESULT::EXTRACT_ERROR;
                if (m_has_progress_bars)
                {
                    m_extract_bar.set_postfix("extraction failed");
                    m_extract_bar.mark_as_completed();
                }
                return false;
            }
        }
        return true;
    }

    bool PackageDownloadExtractTarget::extract_from_cache()
    {
        this->extract();
        m_finished = true;
        return true;
    }

    bool PackageDownloadExtractTarget::validate_extract()
    {
        using std::chrono::nanoseconds;

        if (m_has_progress_bars)
        {
            m_extract_bar.start();
            m_extract_bar.set_postfix("validating");
        }
        validate();

        // Validation
        if (m_validation_result != VALIDATION_RESULT::VALID)
        {
            if (m_has_progress_bars)
            {
                m_extract_bar.set_postfix("validation failed");
            }
            LOG_WARNING << "'" << m_tarball_path.string() << "' validation failed";
            // abort here, but set finished to true
            m_finished = true;
            return true;
        }

        if (m_has_progress_bars)
        {
            m_extract_bar.set_postfix("validated");
        }
        LOG_DEBUG << "'" << m_tarball_path.string() << "' successfully validated";

        bool result = this->extract();
        m_finished = true;
        return result;
    }

    bool PackageDownloadExtractTarget::finalize_callback(const DownloadTarget&)
    {
        if (m_has_progress_bars)
        {
            m_download_bar.repr().postfix.set_value("Downloaded").deactivate();
            m_download_bar.mark_as_completed();
        }

        if (m_target->get_http_status() >= 400)
        {
            LOG_ERROR << "Failed to download package from " << m_url << " (status "
                      << m_target->get_http_status() << ")";
            m_validation_result = VALIDATION_RESULT::UNDEFINED;
            return false;
        }

        LOG_INFO << "Download finished, validating '" << m_tarball_path.string() << "'";
        MainExecutor::instance().schedule(&PackageDownloadExtractTarget::validate_extract, this);

        return true;
    }

    bool PackageDownloadExtractTarget::finished()
    {
        return m_finished;
    }

    auto PackageDownloadExtractTarget::validation_result() const
    {
        return m_validation_result;
    }

    void PackageDownloadExtractTarget::clear_cache() const
    {
        fs::remove_all(m_tarball_path);
        fs::u8path dest_dir = strip_package_extension(m_tarball_path.string());
        if (fs::exists(dest_dir))
        {
            fs::remove_all(dest_dir);
        }
    }

    const std::string& PackageDownloadExtractTarget::name() const
    {
        return m_name;
    }

    std::size_t PackageDownloadExtractTarget::expected_size() const
    {
        return m_expected_size;
    }

    // todo remove cache from this interface
    DownloadTarget* PackageDownloadExtractTarget::target(MultiPackageCache& caches)
    {
        // tarball can be removed, it's fine if only the correct dest dir exists
        // 1. If there is extracted cache, use it, otherwise next.
        // 2. If there is valid tarball, extract it, otherwise next.
        // 3. Run the full download pipeline.

        fs::u8path extracted_cache = caches.get_extracted_dir_path(m_package_info);

        if (extracted_cache.empty())
        {
            fs::u8path tarball_cache = caches.get_tarball_path(m_package_info);
            // Compute the first writable cache and clean its status for the current package
            caches.first_writable_cache(true).clear_query_cache(m_package_info);
            m_cache_path = caches.first_writable_path();

            if (m_has_progress_bars)
            {
                m_extract_bar = Console::instance().add_progress_bar(m_name, 1);
                m_extract_bar.activate_spinner();
                m_extract_bar.set_progress_hook(extract_progress_callback());
                m_extract_bar.set_repr_hook(extract_repr());
                Console::instance().progress_bar_manager().add_label("Extract", m_extract_bar);
            }

            if (!tarball_cache.empty())
            {
                LOG_DEBUG << "Found valid tarball cache at '" << tarball_cache.string() << "'";

                m_tarball_path = tarball_cache / m_filename;
                m_validation_result = VALIDATION_RESULT::VALID;
                MainExecutor::instance().schedule(
                    &PackageDownloadExtractTarget::extract_from_cache,
                    this
                );
                LOG_DEBUG << "Using cached tarball '" << m_filename << "'";
                return nullptr;
            }
            else
            {
                caches.clear_query_cache(m_package_info);
                // need to download this file
                LOG_DEBUG << "Adding '" << m_name << "' to download targets from '" << m_url << "'";

                m_tarball_path = m_cache_path / m_filename;
                m_target = std::make_unique<DownloadTarget>(m_name, m_url, m_tarball_path.string());
                m_target->set_finalize_callback(&PackageDownloadExtractTarget::finalize_callback, this);
                m_target->set_expected_size(m_expected_size);
                if (m_has_progress_bars)
                {
                    m_download_bar = Console::instance().add_progress_bar(m_name, m_expected_size);
                    m_target->set_progress_bar(m_download_bar);
                    Console::instance().progress_bar_manager().add_label("Download", m_download_bar);
                }
                return m_target.get();
            }
        }
        LOG_DEBUG << "Using cached '" << m_name << "'";
        m_finished = true;
        return nullptr;
    }

    /*******************************
     * MTransaction implementation *
     *******************************/

    bool MTransaction::filter(Solvable* s)
    {
        if (m_filter_type == FilterType::none)
        {
            return false;
        }
        bool spec_in_filter = m_filter_name_ids.count(s->name);

        if (m_filter_type == FilterType::keep_only)
        {
            return spec_in_filter;
        }
        else
        {
            return !spec_in_filter;
        }
    }

    MTransaction::MTransaction(
        MPool& pool,
        const std::vector<MatchSpec>& specs_to_remove,
        const std::vector<MatchSpec>& specs_to_install,
        MultiPackageCache& caches
    )
        : m_pool(pool)
        , m_multi_cache(caches)
    {
        // auto& ctx = Context::instance();
        std::vector<PackageInfo> pi_result;

        for (auto& ms : specs_to_install)
        {
            PackageInfo p(ms.name);
            p.url = ms.url;
            p.build_string = ms.build_string;
            p.version = ms.version;
            p.channel = ms.channel;
            p.fn = ms.fn;
            p.subdir = ms.subdir;
            if (ms.brackets.find("md5") != ms.brackets.end())
            {
                p.md5 = ms.brackets.at("md5");
            }
            if (ms.brackets.find("sha256") != ms.brackets.end())
            {
                p.sha256 = ms.brackets.at("sha256");
            }
            pi_result.push_back(p);
        }

        MRepo mrepo = MRepo(m_pool, "__explicit_specs__", pi_result);

        m_pool.create_whatprovides();

        // Just add the packages we want to remove directly to the transaction
        solv::ObjQueue q, job, decision;

        std::vector<std::string> not_found;
        for (auto& s : specs_to_remove)
        {
            job.clear();
            q.clear();

            const Id id = pool_conda_matchspec(m_pool, s.conda_build_form().c_str());
            if (id)
            {
                job.push_back(SOLVER_SOLVABLE_PROVIDES, id);
            }
            selection_solvables(m_pool, job.raw(), q.raw());

            if (q.size() == 0)
            {
                not_found.push_back("\n - " + s.str());
            }
            for (auto& el : q)
            {
                // To remove, these have to be negative
                decision.push_back(-el);
            }
        }

        if (!not_found.empty())
        {
            LOG_ERROR << "Could not find packages to remove:" + join("", not_found) << std::endl;
            throw std::runtime_error("Could not find packages to remove:" + join("", not_found));
        }

        selection_solvables(m_pool, job.raw(), q.raw());
        const bool remove_success = q.size() >= specs_to_remove.size();
        Console::instance().json_write({ { "success", remove_success } });
        Id pkg_id;
        Solvable* solvable;

        // find repo __explicit_specs__ and install all packages from it
        FOR_REPO_SOLVABLES(mrepo.repo(), pkg_id, solvable)
        {
            decision.push_back(pkg_id);
        }

        m_transaction = transaction_create_decisionq(m_pool, decision.raw(), nullptr);
        init();

        m_history_entry = History::UserRequest::prefilled();

        for (auto& s : specs_to_remove)
        {
            m_history_entry.remove.push_back(s.str());
        }
        for (auto& s : specs_to_install)
        {
            m_history_entry.update.push_back(s.str());
        }

        // if no action required, don't even start logging them
        if (!empty())
        {
            Console::instance().json_down("actions");
            Console::instance().json_write(
                { { "PREFIX", Context::instance().prefix_params.target_prefix.string() } }
            );
        }

        m_transaction_context = TransactionContext(
            Context::instance().prefix_params.target_prefix,
            Context::instance().prefix_params.relocate_prefix,
            find_python_version(),
            specs_to_install
        );
    }

    MTransaction::MTransaction(MPool& pool, MSolver& solver, MultiPackageCache& caches)

        : m_pool(pool)
        , m_multi_cache(caches)
    {
        if (!solver.is_solved())
        {
            throw std::runtime_error("Cannot create transaction without calling solver.solve() first."
            );
        }

        m_transaction = solver_create_transaction(solver);
        transaction_order(m_transaction, 0);

        m_history_entry = History::UserRequest::prefilled();

        if (solver.no_deps || solver.only_deps)
        {
            m_filter_type = solver.only_deps ? FilterType::keep_only : FilterType::ignore;
            for (auto& s : solver.install_specs())
            {
                m_filter_name_ids.insert(pool_str2id(m_pool, s.name.c_str(), 0));
            }
            for (auto& s : solver.remove_specs())
            {
                m_filter_name_ids.insert(pool_str2id(m_pool, s.name.c_str(), 0));
            }

            if (solver.only_deps)
            {
                solv::ObjQueue q = {};
                transaction_installedresult(m_transaction, q.raw());
                for (const Id r : q)
                {
                    Solvable* s = pool_id2solvable(m_pool, r);
                    if (m_filter_name_ids.count(s->name))
                    {
                        // add the dependencies of this selected package to the added specs
                        Id* reqp;
                        for (reqp = s->repo->idarraydata + s->requires; *reqp; reqp++)
                        {
                            const char* depname = pool_id2str(m_pool, *reqp);
                            const char* depevr = pool_id2evr(m_pool, *reqp);
                            std::string add_spec;
                            if (depname)
                            {
                                add_spec += depname;
                                if (depevr && strlen(depevr))
                                {
                                    add_spec += " ";
                                    add_spec += depevr;
                                }
                            }
                            m_history_entry.update.push_back(MatchSpec(add_spec).str());
                        }
                    }
                }
            }
        }

        if (solver.only_deps == false)
        {
            auto to_string_vec = [](const std::vector<MatchSpec>& vec) -> std::vector<std::string>
            {
                std::vector<std::string> res;
                for (const auto& el : vec)
                {
                    res.push_back(el.str());
                }
                return res;
            };
            m_history_entry.update = to_string_vec(solver.install_specs());
            m_history_entry.remove = to_string_vec(solver.remove_specs());
        }

        m_force_reinstall = solver.force_reinstall;

        init();
        // if no action required, don't even start logging them
        if (!empty())
        {
            Console::instance().json_down("actions");
            Console::instance().json_write(
                { { "PREFIX", Context::instance().prefix_params.target_prefix.string() } }
            );
        }

        m_transaction_context = TransactionContext(
            Context::instance().prefix_params.target_prefix,
            Context::instance().prefix_params.relocate_prefix,
            find_python_version(),
            solver.install_specs()
        );

        ::Pool* const pool_ptr = static_cast<::Pool*>(m_pool);
        if (m_transaction_context.relink_noarch && pool_ptr->installed != nullptr)
        {
            Id p = 0;
            Solvable* s = nullptr;
            solv::ObjQueue job, q, decision;

            solver_get_decisionqueue(solver, decision.raw());

            FOR_REPO_SOLVABLES(pool_ptr->installed, p, s)
            {
                const char* noarch_type = solvable_lookup_str(s, SOLVABLE_SOURCEARCH);

                if (noarch_type == nullptr)
                {
                    continue;
                }

                if (strcmp(noarch_type, "python") == 0)
                {
                    bool skip_relink = false;
                    for (auto iter = decision.begin(); iter != decision.end(); ++iter)
                    {
                        // if the installed package is kept, delete decision
                        if (*iter == p)
                        {
                            iter = decision.erase(iter);
                            break;
                        }
                        else if (*iter == -p)
                        {
                            // package is _already_ getting delete
                            // in this case, we do not need to manually relink
                            skip_relink = true;
                            break;
                        }
                    }

                    if (skip_relink)
                    {
                        continue;
                    }


                    const auto pkg_info = mk_pkginfo(m_pool, s);
                    const Id id = pool_conda_matchspec(
                        m_pool,
                        fmt::format("{} {} {}", pkg_info.name, pkg_info.version, pkg_info.build_string)
                            .c_str()
                    );

                    if (id)
                    {
                        job.push_back(SOLVER_SOLVABLE_PROVIDES, id);
                    }

                    selection_solvables(m_pool, job.raw(), q.raw());

                    Id reinstall_id = -1;
                    for (const Id r : q)
                    {
                        auto* xid = pool_id2solvable(m_pool, r);
                        if (xid->repo != pool_ptr->installed)
                        {
                            reinstall_id = r;
                            break;
                        }
                    }

                    if (reinstall_id == -1)
                    {
                        // TODO we should also search the local package cache to make offline
                        // installs work
                        LOG_WARNING << fmt::format(
                            "To upgrade python we need to reinstall noarch",
                            " package {} {} {} but we could not find it in",
                            " any of the loaded channels.",
                            pkg_info.name,
                            pkg_info.version,
                            pkg_info.build_string
                        );
                        continue;
                    }

                    decision.push_back(reinstall_id);
                    decision.push_back(-p);

                    q.clear();
                    job.clear();
                }
            }

            transaction_free(m_transaction);
            m_transaction = transaction_create_decisionq(m_pool, decision.raw(), nullptr);
            transaction_order(m_transaction, 0);

            // init everything again...
            init();
        }
    }

    MTransaction::MTransaction(
        MPool& pool,
        const std::vector<PackageInfo>& packages,
        MultiPackageCache& caches
    )
        : m_pool(pool)
        , m_multi_cache(caches)
    {
        LOG_INFO << "MTransaction::MTransaction - packages already resolved (lockfile)";
        MRepo mrepo = MRepo(m_pool, "__explicit_specs__", packages);
        m_pool.create_whatprovides();

        solv::ObjQueue decision = {};

        Id pkg_id = {};
        Solvable* solvable = nullptr;

        FOR_REPO_SOLVABLES(mrepo.repo(), pkg_id, solvable)
        {
            decision.push_back(pkg_id);
        }

        m_transaction = transaction_create_decisionq(m_pool, decision.raw(), nullptr);
        transaction_order(m_transaction, 0);

        init();

        m_history_entry = History::UserRequest::prefilled();

        std::vector<MatchSpec> specs_to_install;
        for (const auto& pkginfo : packages)
        {
            specs_to_install.push_back(MatchSpec(
                fmt::format("{}=={}={}", pkginfo.name, pkginfo.version, pkginfo.build_string)
            ));
        }

        m_transaction_context = TransactionContext(
            Context::instance().prefix_params.target_prefix,
            Context::instance().prefix_params.relocate_prefix,
            find_python_version(),
            specs_to_install
        );
    }

    MTransaction::~MTransaction()
    {
        LOG_INFO << "Freeing transaction.";
        transaction_free(m_transaction);
    }

    void MTransaction::init()
    {
        m_to_remove.clear();
        m_to_install.clear();
        for (int i = 0; i < m_transaction->steps.count && !is_sig_interrupted(); i++)
        {
            Id p = m_transaction->steps.elements[i];
            Id ttype = transaction_type(m_transaction, p, SOLVER_TRANSACTION_SHOW_ALL);
            Solvable* s = pool_id2solvable(m_pool, p);
            if (filter(s))
            {
                continue;
            }
            switch (ttype)
            {
                case SOLVER_TRANSACTION_DOWNGRADED:
                case SOLVER_TRANSACTION_UPGRADED:
                case SOLVER_TRANSACTION_CHANGED:
                case SOLVER_TRANSACTION_REINSTALLED:
                {
                    m_to_remove.push_back(s);
                    m_to_install.push_back(
                        static_cast<::Pool*>(m_pool)->solvables + transaction_obs_pkg(m_transaction, p)
                    );
                    break;
                }
                case SOLVER_TRANSACTION_ERASE:
                {
                    m_to_remove.push_back(s);
                    break;
                }
                case SOLVER_TRANSACTION_INSTALL:
                {
                    m_to_install.push_back(s);
                    break;
                }
                case SOLVER_TRANSACTION_IGNORE:
                    break;
                default:
                    LOG_ERROR << "Exec case not handled: " << ttype;
                    break;
            }
        }
    }

    // TODO rewrite this in terms of `m_transaction`
    std::pair<std::string, std::string> MTransaction::find_python_version()
    {
        // We need to find the python version that will be there after this
        // Transaction is finished in order to compile the noarch packages correctly,
        // for example

        std::string installed_py_ver, new_py_ver;
        Id python = pool_str2id(m_pool, "python", 0);

        for (Solvable* s : m_to_install)
        {
            if (s->name == python)
            {
                new_py_ver = pool_id2str(m_pool, s->evr);
                LOG_INFO << "Found python version in packages to be installed " << new_py_ver;
                break;
            }
        }

        ::Pool* const pool_ptr = m_pool;
        if (pool_ptr->installed != nullptr)
        {
            Id p;
            Solvable* s;

            FOR_REPO_SOLVABLES(pool_ptr->installed, p, s)
            {
                if (s->name == python)
                {
                    installed_py_ver = pool_id2str(m_pool, s->evr);
                    LOG_INFO << "Found python in installed packages " << installed_py_ver;
                    break;
                }
            }
        }
        // if we do not install a new python version but keep the current one
        if (new_py_ver.empty())
        {
            new_py_ver = installed_py_ver;
        }
        return std::make_pair(new_py_ver, installed_py_ver);
    }

    class TransactionRollback
    {
    public:

        void record(const UnlinkPackage& unlink)
        {
            m_unlink_stack.push(unlink);
        }

        void record(const LinkPackage& link)
        {
            m_link_stack.push(link);
        }

        void rollback()
        {
            while (!m_link_stack.empty())
            {
                m_link_stack.top().undo();
                m_link_stack.pop();
            }

            while (!m_unlink_stack.empty())
            {
                m_unlink_stack.top().undo();
                m_unlink_stack.pop();
            }
        }

    private:

        std::stack<UnlinkPackage> m_unlink_stack;
        std::stack<LinkPackage> m_link_stack;
    };

    bool MTransaction::execute(PrefixData& prefix)
    {
        auto& ctx = Context::instance();

        // JSON output
        // back to the top level if any action was required
        if (!empty())
        {
            Console::instance().json_up();
        }
        Console::instance().json_write({ { "dry_run", ctx.dry_run },
                                         { "prefix", ctx.prefix_params.target_prefix.string() } });
        if (empty())
        {
            Console::instance().json_write({ { "message",
                                               "All requested packages already installed" } });
        }

        if (ctx.dry_run)
        {
            Console::stream() << "Dry run. Not executing the transaction.";
            return true;
        }

        auto lf = LockFile(ctx.prefix_params.target_prefix / "conda-meta");
        clean_trash_files(ctx.prefix_params.target_prefix, false);

        Console::stream() << "\nTransaction starting";
        fetch_extract_packages();

        if (ctx.download_only)
        {
            Console::stream(
            ) << "Download only - packages are downloaded and extracted. Skipping the linking phase.";
            return true;
        }

        History::UserRequest ur = History::UserRequest::prefilled();

        TransactionRollback rollback;

        for (int i = 0; i < m_transaction->steps.count && !is_sig_interrupted(); i++)
        {
            Id p = m_transaction->steps.elements[i];
            Id ttype = transaction_type(m_transaction, p, SOLVER_TRANSACTION_SHOW_ALL);
            Solvable* s = pool_id2solvable(m_pool, p);

            if (filter(s))
            {
                continue;
            }

            switch (ttype)
            {
                case SOLVER_TRANSACTION_DOWNGRADED:
                case SOLVER_TRANSACTION_UPGRADED:
                case SOLVER_TRANSACTION_CHANGED:
                case SOLVER_TRANSACTION_REINSTALLED:
                {
                    Solvable* s2 = static_cast<::Pool*>(m_pool)->solvables
                                   + transaction_obs_pkg(m_transaction, p);

                    const PackageInfo package_to_unlink = mk_pkginfo(m_pool, s);
                    const PackageInfo package_to_link = mk_pkginfo(m_pool, s2);

                    Console::stream() << "Changing " << package_to_unlink.str() << " ==> "
                                      << package_to_link.str();

                    const fs::u8path ul_cache_path(
                        m_multi_cache.get_extracted_dir_path(package_to_unlink)
                    );

                    const fs::u8path l_cache_path(
                        m_multi_cache.get_extracted_dir_path(package_to_link, false)
                    );

                    UnlinkPackage up(package_to_unlink, ul_cache_path, &m_transaction_context);
                    up.execute();
                    rollback.record(up);

                    LinkPackage lp(package_to_link, l_cache_path, &m_transaction_context);
                    lp.execute();
                    rollback.record(lp);

                    m_history_entry.unlink_dists.push_back(package_to_unlink.long_str());
                    m_history_entry.link_dists.push_back(package_to_link.long_str());

                    break;
                }
                case SOLVER_TRANSACTION_ERASE:
                {
                    PackageInfo package_info = mk_pkginfo(m_pool, s);
                    Console::stream() << "Unlinking " << package_info.str();
                    const fs::u8path cache_path(m_multi_cache.get_extracted_dir_path(package_info));
                    UnlinkPackage up(package_info, cache_path, &m_transaction_context);
                    up.execute();
                    rollback.record(up);
                    m_history_entry.unlink_dists.push_back(package_info.long_str());
                    break;
                }
                case SOLVER_TRANSACTION_INSTALL:
                {
                    PackageInfo package_info = mk_pkginfo(m_pool, s);
                    Console::stream() << "Linking " << package_info.str();
                    const fs::u8path cache_path(
                        m_multi_cache.get_extracted_dir_path(package_info, false)
                    );
                    LinkPackage lp(package_info, cache_path, &m_transaction_context);
                    lp.execute();
                    rollback.record(lp);
                    m_history_entry.link_dists.push_back(package_info.long_str());
                    break;
                }
                case SOLVER_TRANSACTION_IGNORE:
                    break;
                default:
                    LOG_ERROR << "Exec case not handled: " << ttype;
                    break;
            }
        }

        bool interrupted = is_sig_interrupted();
        if (interrupted)
        {
            Console::stream() << "Transaction interrupted, rollbacking";
            rollback.rollback();
        }
        else
        {
            LOG_INFO << "Waiting for pyc compilation to finish";
            m_transaction_context.wait_for_pyc_compilation();

            // Get the name of the executable used directly from the command.
            const auto executable = ctx.command_params.is_micromamba ? "micromamba" : "mamba";

            // Get the name of the environment
            const auto environment = env_name(ctx.prefix_params.target_prefix);

            Console::stream() << "\nTransaction finished\n\n"
                              << "To activate this environment, use:\n\n"
                              << "    " << executable << " activate " << environment << "\n\n"
                              << "Or to execute a single command in this environment, use:\n\n"
                              << "    " << executable
                              << " run "
                              // Use -n or -p depending on if the env_name is a full prefix or just
                              // a name.
                              << (environment == ctx.prefix_params.target_prefix ? "-p " : "-n ")
                              << environment << " mycommand\n";


            prefix.history().add_entry(m_history_entry);
        }
        return !interrupted;
    }

    auto MTransaction::to_conda() -> to_conda_type
    {
        to_install_type to_install_structured;
        to_remove_type to_remove_structured;

        for (Solvable* s : m_to_remove)
        {
            const char* mediafile = solvable_lookup_str(s, SOLVABLE_MEDIAFILE);
            to_remove_structured.emplace_back(s->repo->name, mediafile);
        }

        for (Solvable* s : m_to_install)
        {
            const char* mediafile = solvable_lookup_str(s, SOLVABLE_MEDIAFILE);
            std::string s_json = solvable_to_json(m_pool, s).dump(4);

            std::string channel;
            if (const char* str = solvable_lookup_str(s, SOLVABLE_PACKAGER))
            {
                channel = str;
            }
            else
            {
                channel = s->repo->name;  // note this can and should be <unknown> when
                                          // e.g. installing from a tarball
            }

            to_install_structured.emplace_back(channel, mediafile, s_json);
        }

        to_specs_type specs;
        std::get<0>(specs) = m_history_entry.update;
        std::get<1>(specs) = m_history_entry.remove;

        return std::make_tuple(specs, to_install_structured, to_remove_structured);
    }

    void MTransaction::log_json()
    {
        std::vector<nlohmann::json> to_fetch, to_link, to_unlink;

        for (Solvable* s : m_to_install)
        {
            if (!need_pkg_download(mk_pkginfo(m_pool, s), m_multi_cache))
            {
                to_link.push_back(solvable_to_json(m_pool, s));
            }
            else
            {
                to_fetch.push_back(solvable_to_json(m_pool, s));
                to_link.push_back(solvable_to_json(m_pool, s));
            }
        }

        for (Solvable* s : m_to_remove)
        {
            to_unlink.push_back(solvable_to_json(m_pool, s));
        }

        auto add_json = [](const auto& jlist, const char* s)
        {
            if (!jlist.empty())
            {
                Console::instance().json_down(s);
                for (nlohmann::json j : jlist)
                {
                    Console::instance().json_append(j);
                }
                Console::instance().json_up();
            }
        };

        add_json(to_fetch, "FETCH");
        add_json(to_link, "LINK");
        add_json(to_unlink, "UNLINK");
    }

    bool MTransaction::fetch_extract_packages()
    {
        std::vector<std::unique_ptr<PackageDownloadExtractTarget>> targets;
        MultiDownloadTarget multi_dl;

        auto& pbar_manager = Console::instance().init_progress_bar_manager(ProgressBarMode::aggregated
        );
        auto& aggregated_pbar_manager = dynamic_cast<AggregatedBarManager&>(pbar_manager);

        auto& ctx = Context::instance();
        DownloadExtractSemaphore::set_max(ctx.threads_params.extract_threads);

        if (ctx.experimental && ctx.verify_artifacts)
        {
            LOG_INFO << "Content trust is enabled, package(s) signatures will be verified";
        }

        for (auto& s : m_to_install)
        {
            std::string const s_url = raw_str_or_empty(
                repo_lookup_str(s->repo, SOLVID_META, SOLVABLE_URL)
            );

            if (ctx.experimental && ctx.verify_artifacts)
            {
                const auto& repo_checker = make_channel(s_url).repo_checker(m_multi_cache);
                const auto pkg_info = mk_pkginfo(m_pool, s);
                repo_checker.verify_package(
                    pkg_info.json_signable(),
                    nlohmann::json::parse(pkg_info.signatures)
                );

                LOG_DEBUG << "'" << pkg_info.name << "' trusted from '" << s_url << "'";
            }

            targets.emplace_back(std::make_unique<PackageDownloadExtractTarget>(mk_pkginfo(m_pool, s))
            );
            DownloadTarget* download_target = targets.back()->target(m_multi_cache);
            if (download_target != nullptr)
            {
                multi_dl.add(download_target);
            }
        }

        if (ctx.experimental && ctx.verify_artifacts)
        {
            auto out = Console::stream();
            fmt::print(
                out,
                "Content trust verifications successful, {} ",
                fmt::styled("package(s) are trusted", Context::instance().graphics_params.palette.safe)
            );
            LOG_INFO << "All package(s) are trusted";
        }

        if (!(ctx.graphics_params.no_progress_bars || ctx.output_params.json
              || ctx.output_params.quiet))
        {
            interruption_guard g([]() { Console::instance().progress_bar_manager().terminate(); });

            auto* dl_bar = aggregated_pbar_manager.aggregated_bar("Download");
            if (dl_bar)
            {
                dl_bar->set_repr_hook(
                    [=](ProgressBarRepr& repr) -> void
                    {
                        auto active_tasks = dl_bar->active_tasks().size();
                        if (active_tasks == 0)
                        {
                            repr.prefix.set_value(fmt::format("{:<16}", "Downloading"));
                            repr.postfix.set_value(fmt::format("{:<25}", ""));
                        }
                        else
                        {
                            repr.prefix.set_value(fmt::format(
                                "{:<11} {:>4}",
                                "Downloading",
                                fmt::format("({})", active_tasks)
                            ));
                            repr.postfix.set_value(fmt::format("{:<25}", dl_bar->last_active_task()));
                        }
                        repr.current.set_value(fmt::format(
                            "{:>7}",
                            to_human_readable_filesize(double(dl_bar->current()), 1)
                        ));
                        repr.separator.set_value("/");

                        std::string total_str;
                        if (dl_bar->total() == std::numeric_limits<std::size_t>::max())
                        {
                            total_str = "??.?MB";
                        }
                        else
                        {
                            total_str = to_human_readable_filesize(double(dl_bar->total()), 1);
                        }
                        repr.total.set_value(fmt::format("{:>7}", total_str));

                        auto speed = dl_bar->avg_speed(std::chrono::milliseconds(500));
                        repr.speed.set_value(
                            speed
                                ? fmt::format("@ {:>7}/s", to_human_readable_filesize(double(speed), 1))
                                : ""
                        );
                    }
                );
            }

            auto* extract_bar = aggregated_pbar_manager.aggregated_bar("Extract");
            if (extract_bar)
            {
                extract_bar->set_repr_hook(
                    [=](ProgressBarRepr& repr) -> void
                    {
                        auto active_tasks = extract_bar->active_tasks().size();
                        if (active_tasks == 0)
                        {
                            repr.prefix.set_value(fmt::format("{:<16}", "Extracting"));
                            repr.postfix.set_value(fmt::format("{:<25}", ""));
                        }
                        else
                        {
                            repr.prefix.set_value(fmt::format(
                                "{:<11} {:>4}",
                                "Extracting",
                                fmt::format("({})", active_tasks)
                            ));
                            repr.postfix.set_value(
                                fmt::format("{:<25}", extract_bar->last_active_task())
                            );
                        }
                        repr.current.set_value(fmt::format("{:>3}", extract_bar->current()));
                        repr.separator.set_value("/");

                        std::string total_str;
                        if (extract_bar->total() == std::numeric_limits<std::size_t>::max())
                        {
                            total_str = "?";
                        }
                        else
                        {
                            total_str = std::to_string(extract_bar->total());
                        }
                        repr.total.set_value(fmt::format("{:>3}", total_str));
                    }
                );
            }

            pbar_manager.start();
            pbar_manager.watch_print();
        }

        bool downloaded = multi_dl.download(MAMBA_DOWNLOAD_FAILFAST | MAMBA_DOWNLOAD_SORT);
        bool all_valid = true;

        if (!downloaded)
        {
            LOG_ERROR << "Download didn't finish!";
            return false;
        }
        // make sure that all targets have finished extracting
        while (!is_sig_interrupted())
        {
            bool all_finished = true;
            for (const auto& t : targets)
            {
                if (!t->finished())
                {
                    all_finished = false;
                    break;
                }
            }
            if (all_finished)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!(ctx.graphics_params.no_progress_bars || ctx.output_params.json
              || ctx.output_params.quiet))
        {
            pbar_manager.terminate();
            pbar_manager.clear_progress_bars();
        }

        for (const auto& t : targets)
        {
            if (t->validation_result() != PackageDownloadExtractTarget::VALIDATION_RESULT::VALID
                && t->validation_result()
                       != PackageDownloadExtractTarget::VALIDATION_RESULT::UNDEFINED)
            {
                t->clear_cache();
                all_valid = false;
                throw std::runtime_error(
                    std::string("Found incorrect download: ") + t->name() + ". Aborting"
                );
            }
        }

        return !is_sig_interrupted() && downloaded && all_valid;
    }

    bool MTransaction::empty()
    {
        return m_to_install.size() == 0 && m_to_remove.size() == 0;
    }

    bool MTransaction::prompt()
    {
        print();
        if (Context::instance().dry_run || empty())
        {
            return true;
        }

        return Console::prompt("Confirm changes", 'y');
    }

    void MTransaction::print()
    {
        const auto& ctx = Context::instance();

        if (ctx.output_params.json)
        {
            return;
        }

        Console::instance().print("Transaction\n");
        Console::stream() << "  Prefix: " << ctx.prefix_params.target_prefix.string() << "\n";

        // check size of transaction
        if (empty())
        {
            if (m_history_entry.update.size())
            {
                Console::instance().print("  All requested packages already installed\n");
            }
            else if (m_history_entry.remove.size())
            {
                // There was no remove events but we still have remove specs treated:
                // The packages to remove were not found in the environment.
                Console::instance().print("  Failure: packages to remove not found in the environment:\n"
                );
                for (const auto& entry : m_history_entry.remove)
                {
                    Console::instance().print(fmt::format("  - {}\n", entry));
                }
            }
            else
            {
                Console::instance().print("  Nothing to do\n");
            }
            return;
        }

        if (m_history_entry.update.size())
        {
            Console::instance().print("  Updating specs:\n");
            for (auto& s : m_history_entry.update)
            {
                Console::stream() << "   - " << s;
            }
        }

        if (m_history_entry.remove.size())
        {
            Console::instance().print("  Removing specs:\n");
            for (auto& s : m_history_entry.remove)
            {
                Console::stream() << "   - " << s;
            }
        }
        Console::stream() << "\n";
        if (m_history_entry.update.empty() && m_history_entry.remove.empty())
        {
            Console::instance().print("  No specs added or removed.\n");
        }

        printers::Table t({ "Package", "Version", "Build", "Channel", "Size" });
        t.set_alignment({ printers::alignment::left,
                          printers::alignment::right,
                          printers::alignment::left,
                          printers::alignment::left,
                          printers::alignment::right });
        t.set_padding({ 2, 2, 2, 2, 5 });
        solv::ObjQueue classes = {};
        solv::ObjQueue pkgs = {};

        using rows = std::vector<std::vector<printers::FormattedString>>;

        rows downgraded, upgraded, changed, reinstalled, erased, installed, ignored;
        std::size_t total_size = 0;

        enum struct Status
        {
            install,
            ignore,
            remove
        };
        auto format_row =
            [this, &ctx, &total_size](rows& r, Solvable* s, Status status, std::string diff)
        {
            std::size_t dlsize = solvable_lookup_num(s, SOLVABLE_DOWNLOADSIZE, SIZE_MAX);
            printers::FormattedString dlsize_s;
            if (dlsize != SIZE_MAX)
            {
                if (status == Status::ignore)
                {
                    dlsize_s.s = "Ignored";
                }
                else
                {
                    if (!need_pkg_download(mk_pkginfo(m_pool, s), m_multi_cache))
                    {
                        dlsize_s.s = "Cached";
                        dlsize_s.style = ctx.graphics_params.palette.addition;
                    }
                    else
                    {
                        std::stringstream ss;
                        to_human_readable_filesize(ss, double(dlsize));
                        dlsize_s.s = ss.str();
                        // Hacky hacky
                        if (status == Status::install)
                        {
                            total_size += dlsize;
                        }
                    }
                }
            }
            printers::FormattedString name;
            name.s = fmt::format("{} {}", diff, pool_id2str(m_pool, s->name));
            if (status == Status::install)
            {
                name.style = ctx.graphics_params.palette.addition;
            }
            else if (status == Status::ignore)
            {
                name.style = ctx.graphics_params.palette.ignored;
            }
            else if (status == Status::remove)
            {
                name.style = ctx.graphics_params.palette.deletion;
            }
            const char* build_string = solvable_lookup_str(s, SOLVABLE_BUILDFLAVOR);

            std::string channel;
            if (const char* str = solvable_lookup_str(s, SOLVABLE_PACKAGER))
            {
                if (std::string_view(str) == "explicit_specs")
                {
                    channel = solvable_lookup_str(s, SOLVABLE_MEDIAFILE);
                }
                else
                {
                    channel = make_channel(str).canonical_name();
                }
            }
            else
            {
                // note this can and should be <unknown> when
                // e.g. installing from a tarball
                assert(s->repo != nullptr);
                channel = s->repo->name;
                assert(channel != "__explicit_specs__");
            }

            r.push_back({ name,
                          printers::FormattedString(pool_id2str(m_pool, s->evr)),
                          printers::FormattedString(build_string ? build_string : ""),
                          printers::FormattedString(cut_repo_name(channel)),
                          dlsize_s });
        };


        ::Solvable* const solvables = static_cast<::Pool*>(m_pool)->solvables;
        int mode = SOLVER_TRANSACTION_SHOW_OBSOLETES | SOLVER_TRANSACTION_OBSOLETE_IS_UPGRADE;
        transaction_classify(m_transaction, mode, classes.raw());
        for (std::size_t n_classes = classes.size(), i = 0; i < n_classes; i += 4)
        {
            const Id cls = classes.at(i);
            transaction_classify_pkgs(
                m_transaction,
                mode,
                cls,
                classes.at(i + 2),
                classes.at(i + 3),
                pkgs.raw()
            );

            for (const Id p : pkgs)
            {
                Solvable* s = solvables + p;

                if (filter(s))
                {
                    format_row(ignored, s, Status::ignore, "=");
                    continue;
                }
                switch (cls)
                {
                    case SOLVER_TRANSACTION_UPGRADED:
                        format_row(upgraded, s, Status::remove, "-");
                        format_row(
                            upgraded,
                            solvables + transaction_obs_pkg(m_transaction, p),
                            Status::install,
                            "+"
                        );
                        break;
                    case SOLVER_TRANSACTION_CHANGED:
                        format_row(changed, s, Status::remove, "-");
                        format_row(
                            changed,
                            solvables + transaction_obs_pkg(m_transaction, p),
                            Status::install,
                            "+"
                        );
                        break;
                    case SOLVER_TRANSACTION_REINSTALLED:
                        format_row(reinstalled, s, Status::install, "o");
                        break;
                    case SOLVER_TRANSACTION_DOWNGRADED:
                        format_row(downgraded, s, Status::remove, "-");
                        format_row(
                            downgraded,
                            solvables + transaction_obs_pkg(m_transaction, p),
                            Status::install,
                            "+"
                        );
                        break;
                    case SOLVER_TRANSACTION_ERASE:
                        format_row(erased, s, Status::remove, "-");
                        break;
                    case SOLVER_TRANSACTION_INSTALL:
                        format_row(installed, s, Status::install, "+");
                        break;
                    case SOLVER_TRANSACTION_IGNORE:
                        break;
                    case SOLVER_TRANSACTION_VENDORCHANGE:
                    case SOLVER_TRANSACTION_ARCHCHANGE:
                    default:
                        LOG_ERROR << "Print case not handled: " << cls;
                        break;
                }
            }
        }

        std::stringstream summary;
        summary << "Summary:\n\n";
        if (installed.size())
        {
            t.add_rows("Install:", installed);
            summary << "  Install: " << installed.size() << " packages\n";
        }
        if (erased.size())
        {
            t.add_rows("Remove:", erased);
            summary << "  Remove: " << erased.size() << " packages\n";
        }
        if (changed.size())
        {
            t.add_rows("Change:", changed);
            summary << "  Change: " << changed.size() / 2 << " packages\n";
        }
        if (reinstalled.size())
        {
            t.add_rows("Reinstall:", reinstalled);
            summary << "  Reinstall: " << reinstalled.size() << " packages\n";
        }
        if (upgraded.size())
        {
            t.add_rows("Upgrade:", upgraded);
            summary << "  Upgrade: " << upgraded.size() / 2 << " packages\n";
        }
        if (downgraded.size())
        {
            t.add_rows("Downgrade:", downgraded);
            summary << "  Downgrade: " << downgraded.size() / 2 << " packages\n";
        }
        if (ignored.size())
        {
            t.add_rows("Ignored:", ignored);
            summary << "  Ignored: " << ignored.size() << " packages\n";
        }

        summary << "\n  Total download: ";
        to_human_readable_filesize(summary, double(total_size));
        summary << "\n";
        t.add_row({ summary.str() });
        auto out = Console::stream();
        t.print(out);
    }

    MTransaction
    create_explicit_transaction_from_urls(MPool& pool, const std::vector<std::string>& urls, MultiPackageCache& package_caches, std::vector<detail::other_pkg_mgr_spec>&)
    {
        std::vector<MatchSpec> specs_to_install;
        for (auto& u : urls)
        {
            std::string x(strip(u));
            if (x.empty())
            {
                continue;
            }

            std::size_t hash = u.find_first_of('#');
            MatchSpec ms(u.substr(0, hash));

            if (hash != std::string::npos)
            {
                std::string s_hash = u.substr(hash + 1);
                if (starts_with(s_hash, "sha256:"))
                {
                    ms.brackets["sha256"] = s_hash.substr(7);
                }
                else
                {
                    ms.brackets["md5"] = s_hash;
                }
            }
            specs_to_install.push_back(ms);
        }
        return MTransaction(pool, {}, specs_to_install, package_caches);
    }

    MTransaction create_explicit_transaction_from_lockfile(
        MPool& pool,
        const fs::u8path& env_lockfile_path,
        const std::vector<std::string>& categories,
        MultiPackageCache& package_caches,
        std::vector<detail::other_pkg_mgr_spec>& other_specs
    )
    {
        const auto maybe_lockfile = read_environment_lockfile(env_lockfile_path);
        if (!maybe_lockfile)
        {
            throw maybe_lockfile.error();  // NOTE: we cannot return an `un/expected` because
                                           // MTransaction is not move-enabled.
        }

        const auto lockfile_data = maybe_lockfile.value();

        struct
        {
            std::vector<PackageInfo> conda, pip;
        } packages;

        for (const auto& category : categories)
        {
            std::vector<PackageInfo> selected_packages;

            selected_packages = lockfile_data.get_packages_for(
                category,
                Context::instance().platform,
                "conda"
            );
            std::copy(
                selected_packages.begin(),
                selected_packages.end(),
                std::back_inserter(packages.conda)
            );

            if (selected_packages.empty())
            {
                LOG_WARNING << "Selected packages for category '" << category << "' are empty. "
                            << "The lockfile might not be resolved for your platform ("
                            << Context::instance().platform << ").";
            }

            selected_packages = lockfile_data
                                    .get_packages_for(category, Context::instance().platform, "pip");
            std::copy(
                selected_packages.begin(),
                selected_packages.end(),
                std::back_inserter(packages.pip)
            );
        }

        // extract pip packages
        if (!packages.pip.empty())
        {
            std::vector<std::string> pip_specs;
            for (const auto& package : packages.pip)
            {
                pip_specs.push_back(package.name + " @ " + package.url + "#sha256=" + package.sha256);
            }
            other_specs.push_back(
                { "pip --no-deps", pip_specs, fs::absolute(env_lockfile_path.parent_path()).string() }
            );
        }

        return MTransaction{ pool, packages.conda, package_caches };
    }

}  // namespace mamba

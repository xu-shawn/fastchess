#include <matchmaking/tournament/roundrobin/roundrobin.hpp>

#include <chess.hpp>

#include <matchmaking/output/output_factory.hpp>
#include <pgn/pgn_builder.hpp>
#include <util/logger/logger.hpp>
#include <util/rand.hpp>
#include <util/scope_guard.hpp>

namespace fastchess {

RoundRobin::RoundRobin(const stats_map& results) : BaseTournament(results) {
    // Initialize the SPRT test
    sprt_ = SPRT(config::TournamentConfig.get().sprt.alpha, config::TournamentConfig.get().sprt.beta,
                 config::TournamentConfig.get().sprt.elo0, config::TournamentConfig.get().sprt.elo1,
                 config::TournamentConfig.get().sprt.model, config::TournamentConfig.get().sprt.enabled);
}

void RoundRobin::start() {
    Logger::trace("Starting round robin tournament...");

    // If autosave is enabled, save the results every save_interval games
    const auto save_interval = config::TournamentConfig.get().autosaveinterval;

    // Account for the initial matchcount
    auto save_iter = initial_matchcount_ + save_interval;

    BaseTournament::start();

    // Wait for games to finish
    while (match_count_ < total_ && !atomic::stop) {
        if (save_interval > 0 && match_count_ >= save_iter) {
            saveJson();
            save_iter += save_interval;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    pool_.kill();
}

void RoundRobin::startNext() {
    std::lock_guard<std::mutex> lock(game_gen_mutex_);

    if (atomic::stop) return;

    auto match = generator_->next();

    if (!match) {
        Logger::trace("No more matches to generate");
        return;
    }

    std::apply(
        [this](auto&&... args) {
            pool_.enqueue(&RoundRobin::createMatch, this, std::forward<decltype(args)>(args)...);
        },
        match.value());
}

void RoundRobin::create() {
    Logger::trace("Creating matches...");

    total_ = (config::EngineConfigs.get().size() * (config::EngineConfigs.get().size() - 1) / 2) *
             config::TournamentConfig.get().rounds * config::TournamentConfig.get().games;

    for (int i = 0; i < pool_.getNumThreads(); i++) {
        startNext();
    }
}

void RoundRobin::createMatch(std::size_t i, std::size_t j, std::size_t round_id, int g,
                             std::optional<std::size_t> opening_id) {
    assert(g < 2);

    const auto opening        = (*book_)[opening_id];
    const auto first          = config::EngineConfigs.get()[i];
    const auto second         = config::EngineConfigs.get()[j];
    const std::size_t game_id = round_id * config::TournamentConfig.get().games + (g + 1);

    GamePair<EngineConfiguration, EngineConfiguration> configs = {first, second};

    if (game_id % 2 == 0 && !config::TournamentConfig.get().noswap) {
        std::swap(configs.white, configs.black);
    }

    if (config::TournamentConfig.get().reverse) {
        std::swap(configs.white, configs.black);
    }

    // callback functions, do not capture by reference
    const auto start = [this, configs, game_id]() { output_->startGame(configs, game_id, total_); };

    // callback functions, do not capture by reference
    const auto finish = [this, configs, first, second, game_id, round_id](const Stats& stats, const std::string& reason,
                                                                          const engines& engines) {
        const auto& cfg = config::TournamentConfig.get();

        // lock to avoid chaotic output, i.e.
        // Finished game 187 (Engine1 vs Engine2): 0-1 {White loses on time}
        // Finished game 186 (Engine2 vs Engine1): 0-1 {White loses on time}
        // Score of Engine1 vs Engine2: 95 - 92 - 0  [0.508] 187
        // Score of Engine1 vs Engine2: 94 - 92 - 0  [0.505] 186
        std::lock_guard<std::mutex> lock(output_mutex_);

        output_->endGame(configs, stats, reason, game_id);

        if (cfg.report_penta) {
            scoreboard_.updatePair(configs, stats, round_id);
        } else {
            scoreboard_.updateNonPair(configs, stats);
        }

        const auto updated_stats = scoreboard_.getStats(first.name, second.name);

        if (shouldPrintScoreInterval() || allMatchesPlayed()) {
            output_->printResult(updated_stats, first.name, second.name);
        }

        if ((shouldPrintRatingInterval(round_id) && scoreboard_.isPairCompleted(round_id)) || allMatchesPlayed()) {
            output_->printInterval(sprt_, updated_stats, first.name, second.name, engines, cfg.opening.file);
        }

        updateSprtStatus({first, second}, engines);

        match_count_++;
    };

    playGame(configs, start, finish, opening, round_id, game_id);

    if (config::TournamentConfig.get().wait > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(config::TournamentConfig.get().wait));
}

void RoundRobin::updateSprtStatus(const std::vector<EngineConfiguration>& engine_configs, const engines& engines) {
    if (!sprt_.isEnabled()) return;

    const auto stats = scoreboard_.getStats(engine_configs[0].name, engine_configs[1].name);
    const auto llr   = sprt_.getLLR(stats, config::TournamentConfig.get().report_penta);

    if (sprt_.getResult(llr) != SPRT_CONTINUE || match_count_ == total_) {
        atomic::stop = true;

        Logger::info("SPRT test finished: {} {}", sprt_.getBounds(), sprt_.getElo());

        output_->printResult(stats, engine_configs[0].name, engine_configs[1].name);
        output_->printInterval(sprt_, stats, engine_configs[0].name, engine_configs[1].name, engines,
                               config::TournamentConfig.get().opening.file);
        output_->endTournament();
    }
}

}  // namespace fastchess

import QtQuick
import QtTest
import QtPie 1.0

Item {
    width: 800
    height: 600

    // Test the GameHistoryModel directly
    GameHistoryModel {
        id: gameModel
    }

    TestCase {
        name: "GameHistoryModelTests"
        when: windowShown

        function test_initial_event_index() {
            // Model should start at event index 0
            compare(gameModel.currentEventIndex, 0, "Initial event index should be 0");
        }

        function test_initial_total_events() {
            compare(gameModel.totalEvents, 0, "Initial total events should be 0");
        }

        function test_player_scores_initial() {
            compare(gameModel.player1Score, 0, "Player 1 initial score should be 0");
            compare(gameModel.player2Score, 0, "Player 2 initial score should be 0");
        }

        function test_player_names_initial() {
            // Player names should be empty or default before game load
            verify(gameModel.player1Name !== undefined, "Player 1 name should be defined");
            verify(gameModel.player2Name !== undefined, "Player 2 name should be defined");
        }

        function test_board_exists() {
            // Board should exist as a list
            verify(gameModel.board !== undefined, "Board should exist");
            verify(gameModel.board !== null, "Board should not be null");
        }

        function test_board_initially_empty() {
            // Board starts empty before a game is loaded
            compare(gameModel.board.length, 0, "Board should be empty before game load");
        }

        function test_current_rack_exists() {
            // Rack should be a string (possibly empty)
            verify(gameModel.currentRack !== undefined, "Current rack should be defined");
        }

        function test_history_exists() {
            verify(gameModel.history !== undefined, "History should exist");
        }

        function test_bag_count_initial() {
            // Before loading a game, bag count behavior depends on implementation
            verify(gameModel.bagCount !== undefined, "Bag count should be defined");
        }

        function test_unseen_tiles_initial() {
            verify(gameModel.unseenTiles !== undefined, "Unseen tiles should be defined");
        }

        function test_analysis_model_exists() {
            verify(gameModel.analysisModel !== undefined, "Analysis model should exist");
            verify(gameModel.analysisModel !== null, "Analysis model should not be null");
        }

        function test_player_on_turn_initial() {
            // Player on turn should be 0 or 1
            var pot = gameModel.playerOnTurnIndex;
            verify(pot >= 0 && pot <= 1, "Player on turn should be 0 or 1");
        }

        function test_lexicon_name_exists() {
            verify(gameModel.lexiconName !== undefined, "Lexicon name should be defined");
        }

        function test_game_mode_initial() {
            verify(gameModel.gameMode !== undefined, "Game mode should be defined");
        }
    }

    TestCase {
        name: "HistoryItemTests"
        when: windowShown

        function test_history_initially_empty() {
            compare(gameModel.history.length, 0, "History should be empty before game load");
        }

        function test_total_history_items_initial() {
            compare(gameModel.totalHistoryItems, 0, "Total history items should be 0 initially");
        }
    }

    TestCase {
        name: "AnalysisModelTests"
        when: windowShown

        function test_initial_state() {
            compare(gameModel.analysisModel.rowCount(), 0, "Analysis should start empty");
        }
    }
}

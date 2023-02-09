package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/domino14/macondo/ai/runner"
	"github.com/domino14/macondo/automatic"
	"github.com/domino14/macondo/config"
	"github.com/domino14/macondo/game"
	"github.com/domino14/macondo/gcgio"
	pb "github.com/domino14/macondo/gen/api/proto/macondo"
)

func RunComparisonTests() {
	for {
		game := CreateTopEquityStaticGame()
		ok := CompareMovesForGame(game)
		if !ok {
			break
		}
	}
}

func playGameToTurn(g *game.Game, turnNumber int) *game.Game {
	var newGame *game.Game
	newGame, err := game.NewFromHistory(g.History(), g.Rules(), turnNumber)
	if err != nil {
		panic(err)
	}
	return newGame
}

func createMoveMap(g *game.Game, turnNumber int) (map[string]bool, bool) {
	lg := playGameToTurn(g, turnNumber)
	if lg.Playing() != pb.PlayState_PLAYING {
		return nil, false
	}
	agr, err := runner.NewAIGameRunnerFromGame(lg, nil, pb.BotRequest_HASTY_BOT)
	if err != nil {
		panic(err)
	}
	moves := agr.GenerateMoves(1000000)
	moveMap := map[string]bool{}
	for _, move := range moves {
		moveKey := fmt.Sprintf("%d,%s,%s,%d", move.Action(), move.BoardCoords(), move.TilesString(), move.Score())
		moveMap[moveKey] = true
	}
	return moveMap, true
}

func getActualMoves(g *game.Game, turnNumber int) map[string]bool {
	return nil
}

// Performs m1 - m2 and returns the result as a list
func subtractMaps(m1 map[string]bool, m2 map[string]bool) []string {
	for key, _ := range m2 {
		_, exists := m1[key]
		if exists {
			m1[key] = false
		}
	}
	moves := []string{}
	for key, val := range m1 {
		if val {
			moves = append(moves, key)
		}
	}
	return moves
}

func CompareMovesForGame(g *game.Game) bool {
	for i := 0; i < len(g.History().Events); i++ {
		expectedMoves, playing := createMoveMap(g, i)
		if !playing {
			break
		}
		actualMoves := getActualMoves(g, i)

		expectedMovesNotGenerated := subtractMaps(expectedMoves, actualMoves)
		extraneousMovesGenerated := subtractMaps(actualMoves, expectedMoves)

		errString := ""
		if len(expectedMovesNotGenerated) > 0 {
			errString += fmt.Sprintf("%d expected moves not generated:\n", len(expectedMovesNotGenerated))
			errString += fmt.Sprintln(strings.Join(expectedMovesNotGenerated, "\n"))
		}
		if len(extraneousMovesGenerated) > 0 {
			errString += fmt.Sprintf("%d extraneous moves generated:\n", len(extraneousMovesGenerated))
			errString += fmt.Sprintln(strings.Join(extraneousMovesGenerated, "\n"))
		}
		if errString != "" {
			fmt.Printf("Turn %d\n\n", i)
			fmt.Printf("%s\n\n", errString)
			gameAtTurn := playGameToTurn(g, i)
			fmt.Println("\nText Display:")
			fmt.Println(gameAtTurn.ToDisplayText())
			fmt.Println("\nCGP:")
			fmt.Println(gameToCGP(gameAtTurn, true))
			fmt.Println("\nGCG:")
			gcg, err := gcgio.GameHistoryToGCG(g.History(), true)
			if err != nil {
				fmt.Printf("error generating gcg file: %s", err.Error())
			} else {
				fmt.Println(gcg)
			}
			return false
		}
	}
	return true
}

func CreateTopEquityStaticGame() *game.Game {
	runner := automatic.NewGameRunner(nil, &config.Config{
		StrategyParamsPath:        os.Getenv("STRATEGY_PARAMS_PATH"),
		LexiconPath:               os.Getenv("LEXICON_PATH"),
		LetterDistributionPath:    os.Getenv("LETTER_DISTRIBUTION_PATH"),
		DefaultLexicon:            os.Getenv("DEFAULT_LEXICON"),
		DefaultLetterDistribution: "English",
	})
	err := runner.CompVsCompStatic(true)
	if err != nil {
		panic(err)
	}
	return runner.Game()
}

package main

import (
	"fmt"
	"os"
	"os/exec"
	"strings"

	"github.com/domino14/macondo/ai/runner"
	"github.com/domino14/macondo/automatic"
	"github.com/domino14/macondo/config"
	"github.com/domino14/macondo/game"
	"github.com/domino14/macondo/gcgio"
	pb "github.com/domino14/macondo/gen/api/proto/macondo"
	"github.com/domino14/macondo/move"
)

var passMove = "pass"

func RunComparisonTests(threadName string) {
	count := 0
	for {
		count++
		fmt.Printf("%s - Game %d\n", threadName, count)
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

func createMoveMap(g *game.Game) map[string]bool {
	agr, err := runner.NewAIGameRunnerFromGame(g, nil, pb.BotRequest_HASTY_BOT)
	if err != nil {
		panic(err)
	}
	moves := agr.GenerateMoves(1000000)
	moveMap := map[string]bool{}
	// Macondo only includes the pass move if there are
	// no other moves available. Magpie will always include
	// the pass move. So if the pass move was not produced,
	// add it to the move map anyway so the two maps are
	// equivalent.
	passMoveIncluded := false
	for _, mv := range moves {
		var moveKey string
		if mv.Action() == move.MoveTypePass {
			moveKey = passMove
			passMoveIncluded = true
		} else {
			moveKey = fmt.Sprintf("%d,%s,%s,%d", mv.Action(), mv.BoardCoords(), mv.TilesString(), mv.Score())
		}
		moveMap[moveKey] = true
	}
	if !passMoveIncluded {
		moveMap[passMove] = true
	}
	return moveMap
}

func getActualMoves(g *game.Game) map[string]bool {
	cgp := gameToCGP(g, true)
	gaddag := "data/lexica/CSW21.gaddag"
	alphabet := "data/lexica/CSW21.alph"
	dist := "data/letterdistributions/english.dist"
	laddag := "/data/lexica/CSW21.laddag"
	cmd := []string{
		"gen",
		"-g", "../core/" + gaddag,
		"-a", "../core/" + alphabet,
		"-d", "../core/" + dist,
		"-l1", "../core/" + laddag,
		"-r1", "all",
		"-s1", "equity",
		"-c", cgp,
	}
	outBytes, err := exec.Command("../core/bin/magpie_test", cmd...).Output()
	if err != nil {
		fmt.Println("Command to run from core to reproduce:")
		fmt.Printf("./bin/magpie_test gen -g '%s' -a '%s' -d '%s' -l '%s' -r all -s equity -c '%s'\n", gaddag, alphabet, dist, laddag, cgp)
		fmt.Println("panicked on game")
		printGameInfo(g)
		panic(err)
	}
	output := string(outBytes)
	moves := strings.Split(output, "\n")
	moveMap := map[string]bool{}
	for _, mv := range moves {
		if mv != "" {
			moveMap[mv] = true
		}
	}
	return moveMap
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

func printGameInfo(g *game.Game) {
	fmt.Println("\nText Display:")
	fmt.Println(g.ToDisplayText())
	fmt.Println("\nCGP:")
	fmt.Println(gameToCGP(g, true))
	fmt.Println("\nGCG:")
	gcg, err := gcgio.GameHistoryToGCG(g.History(), true)
	if err != nil {
		fmt.Printf("error generating gcg file: %s", err.Error())
	} else {
		fmt.Println(gcg)
	}
}

func CompareMovesForGame(g *game.Game) bool {
	for i := 0; i < len(g.History().Events); i++ {
		gameAtTurn := playGameToTurn(g, i)
		if gameAtTurn.Playing() != pb.PlayState_PLAYING {
			return true
		}
		expectedMoves := createMoveMap(gameAtTurn)
		actualMoves := getActualMoves(gameAtTurn)

		expectedMovesNotGenerated := subtractMaps(expectedMoves, actualMoves)
		extraneousMovesGenerated := subtractMaps(actualMoves, expectedMoves)

		errString := ""
		if len(expectedMovesNotGenerated) > 0 {
			errString += fmt.Sprintf("%d expected moves not generated:\n", len(expectedMovesNotGenerated))
			for i := 0; i < len(expectedMovesNotGenerated); i++ {
				errString += fmt.Sprintf(">%s<", expectedMovesNotGenerated[i])
			}
		}
		if len(extraneousMovesGenerated) > 0 {
			errString += fmt.Sprintf("%d extraneous moves generated:\n", len(extraneousMovesGenerated))
			for i := 0; i < len(extraneousMovesGenerated); i++ {
				errString += fmt.Sprintf(">%s<", extraneousMovesGenerated[i])
			}
		}
		if errString != "" {
			fmt.Printf("Turn %d\n\n", i)
			fmt.Printf("%s\n\n", errString)
			gameAtTurn := playGameToTurn(g, i)
			printGameInfo(gameAtTurn)
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

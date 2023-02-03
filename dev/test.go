package main

import (
	"fmt"
	"os"

	"github.com/domino14/macondo/ai/runner"
	"github.com/domino14/macondo/automatic"
	"github.com/domino14/macondo/config"
	"github.com/domino14/macondo/game"
	pb "github.com/domino14/macondo/gen/api/proto/macondo"
)

func RunComparisonTests() {
	game := CreateTopEquityStaticGame()
	CompareMovesForGame(game)
}

func CompareMovesForGame(g *game.Game) {
	var err error
	var lg *game.Game
	var agr *runner.AIGameRunner
	for i := 0; i < len(g.History().Events); i++ {
		lg, err = game.NewFromHistory(g.History(), g.Rules(), i)
		if err != nil {
			panic(err)
		}
		if lg.Playing() != pb.PlayState_PLAYING {
			break
		}
		agr, err = runner.NewAIGameRunnerFromGame(lg, nil, pb.BotRequest_HASTY_BOT)
		if err != nil {
			panic(err)
		}
		moves := agr.GenerateMoves(1000000)
		moveMap := map[string]bool{}
		for _, move := range moves {
			moveKey := fmt.Sprintf("%s,%s,%s,%d", move.MoveTypeString(), move.BoardCoords(), move.TilesString(), move.Score())
			moveMap[moveKey] = true
		}
	}
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

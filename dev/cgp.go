package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/domino14/macondo/ai/runner"
	"github.com/domino14/macondo/alphabet"
	"github.com/domino14/macondo/board"
	"github.com/domino14/macondo/config"
	"github.com/domino14/macondo/game"
	pb "github.com/domino14/macondo/gen/api/proto/macondo"
)

var DefaultConfig = config.Config{
	StrategyParamsPath:        os.Getenv("STRATEGY_PARAMS_PATH"),
	LexiconPath:               os.Getenv("LEXICON_PATH"),
	LetterDistributionPath:    os.Getenv("LETTER_DISTRIBUTION_PATH"),
	DefaultLexicon:            os.Getenv("DEFAULT_LEXICON"),
	DefaultLetterDistribution: "English",
}

func ConvertMacondoBoardStringToCGP(boardString string, alph *alphabet.Alphabet, player0Rack string, player1Rack string, player0Points int, player1Points int, scorelessTurns int, lexicon string) string {
	b := board.MakeBoard(board.CrosswordGameBoard)
	b.SetToGame(alph, board.VsWho(boardString))

	b.Dim()
	var cgp strings.Builder
	cgp.WriteString(gameBoardToCGP(b, alph))

	// Write the racks
	fmt.Fprint(&cgp, gameStateToCGPString(
		player0Rack,
		player1Rack,
		player0Points,
		player1Points,
		scorelessTurns,
		lexicon))
	return cgp.String()
}

func PrintCGPTurns(racks []string) {
	cgps := ConvertRacksToCGP(racks)
	for i := 0; i < len(cgps); i++ {
		fmt.Printf("\"%s\"\n", cgps[i])
	}
}

func ConvertRacksToCGP(racks []string) []string {
	players := []*pb.PlayerInfo{
		{Nickname: "A", RealName: "Alice"},
		{Nickname: "B", RealName: "Bob"},
	}
	rules, err := game.NewBasicGameRules(
		&DefaultConfig,
		"CSW21",
		board.CrosswordGameLayout,
		"english",
		game.CrossScoreAndSet,
		"")
	if err != nil {
		panic(err)
	}

	macondoGame, err := game.NewGame(rules, players)
	if err != nil {
		panic(err)
	}

	macondoGame.StartGame()
	// Overwrite the player on turn to be JD:
	alph := macondoGame.Alphabet()
	macondoGame.SetPlayerOnTurn(0)
	cgps := []string{}
	for i := 0; i < len(racks); i++ {
		rack := racks[i]
		macondoGame.SetRackFor(i%2, alphabet.RackFromString(rack, alph))
		gameRunner, err := runner.NewAIGameRunnerFromGame(macondoGame, &DefaultConfig, pb.BotRequest_HASTY_BOT)
		if err != nil {
			panic(err)
		}
		moves := gameRunner.GenerateMoves(1)
		_, err = macondoGame.ValidateMove(moves[0])
		if err != nil {
			panic(err)
		}
		macondoGame.PlayMove(moves[0], true, 0)
		fmt.Println(macondoGame.ToDisplayText())
		cgps = append(cgps, gameToCGP(macondoGame, false))
		if macondoGame.Playing() != pb.PlayState_PLAYING {
			break
		}
	}
	return cgps
}

func FindRandomSixPass() *game.Game {
	n := 0
	for n < 1000000 {
		randomTopEquityGame := RandomTopEquity()
		n++
		if n%100 == 0 {
			fmt.Printf("%d\n", n)
			if randomTopEquityGame == nil {
				fmt.Println("game is nil")
				continue
			} else {
				fmt.Println(randomTopEquityGame.ToDisplayText())
			}
		}
		if randomTopEquityGame != nil && randomTopEquityGame.ScorelessTurns() == 6 {
			return randomTopEquityGame
		}
	}
	return nil
}

func FindRandomStandard() *game.Game {
	n := 0
	for n < 1000000 {
		randomTopEquityGame := RandomTopEquity()
		n++
		if n%100 == 0 {
			fmt.Printf("%d\n", n)
			if randomTopEquityGame == nil {
				fmt.Println("game is nil")
				continue
			} else {
				fmt.Println(randomTopEquityGame.ToDisplayText())
			}
		}
		if randomTopEquityGame != nil {
			return randomTopEquityGame
		}
	}
	return nil
}

func RandomTopEquity() *game.Game {
	players := []*pb.PlayerInfo{
		{Nickname: "A", RealName: "Alice"},
		{Nickname: "B", RealName: "Bob"},
	}
	rules, err := game.NewBasicGameRules(
		&DefaultConfig,
		"CSW21",
		board.CrosswordGameLayout,
		"english",
		game.CrossScoreAndSet,
		"")
	if err != nil {
		panic(err)
	}

	macondoGame, err := game.NewGame(rules, players)
	if err != nil {
		panic(err)
	}

	macondoGame.StartGame()
	// Overwrite the player on turn to be JD:
	macondoGame.SetPlayerOnTurn(0)
	for macondoGame.Playing() == pb.PlayState_PLAYING {
		macondoGame.SetRandomRack(macondoGame.PlayerOnTurn())
		gameRunner, err := runner.NewAIGameRunnerFromGame(macondoGame, &DefaultConfig, pb.BotRequest_HASTY_BOT)
		if err != nil {
			panic(err)
		}
		moves := gameRunner.GenerateMoves(2)
		if len(moves) > 1 && moves[0].Equity()-0.01 < moves[1].Equity() {
			return nil
		}
		if err != nil {
			panic(err)
		}
		macondoGame.PlayMove(moves[0], true, 0)
	}

	return macondoGame
}

func gameToCGP(g *game.Game, includeRacks bool) string {
	board := g.Board()
	board.Dim()
	var cgp strings.Builder
	cgp.WriteString(gameBoardToCGP(board, g.Alphabet()))

	playerOnTurnRack := ""
	playerNotOnTurnRack := ""

	playerOnTurn := g.PlayerOnTurn()
	if includeRacks {
		playerOnTurnRack = g.RackFor(playerOnTurn).String()
		playerNotOnTurnRack = g.RackFor(1 - playerOnTurn).String()
	}
	// Write the racks
	fmt.Fprint(&cgp, gameStateToCGPString(
		playerOnTurnRack,
		playerNotOnTurnRack,
		g.PointsFor(playerOnTurn),
		g.PointsFor(1-playerOnTurn),
		g.ScorelessTurns(),
		g.LexiconName()))
	return cgp.String()
}

func gameBoardToCGP(board *board.GameBoard, alphabet *alphabet.Alphabet) string {
	board.Dim()
	var cgp strings.Builder

	// Write the board
	for i := 0; i < board.Dim(); i++ {
		consecutiveEmptySquares := 0
		for j := 0; j < board.Dim(); j++ {
			if !board.HasLetter(i, j) {
				consecutiveEmptySquares++
			} else {
				if consecutiveEmptySquares > 0 {
					fmt.Fprintf(&cgp, "%d", consecutiveEmptySquares)
					consecutiveEmptySquares = 0
				}
				cgp.WriteString(string(board.GetLetter(i, j).UserVisible(alphabet)))
			}
		}
		if consecutiveEmptySquares > 0 {
			fmt.Fprintf(&cgp, "%d", consecutiveEmptySquares)
			consecutiveEmptySquares = 0
		}
		if i != board.Dim()-1 {
			cgp.WriteString("/")
		}
	}
	return cgp.String()
}

func gameStateToCGPString(player0Rack string, player1Rack string, player0Points int, player1Points int, scorelessTurns int, lexicon string) string {
	return fmt.Sprintf(" %s/%s %d/%d %d lex %s;",
		player0Rack,
		player1Rack,
		player0Points,
		player1Points,
		scorelessTurns,
		lexicon)
}

package main

import (
	"fmt"
	"os/exec"
	"strconv"
	"strings"

	"github.com/domino14/macondo/board"
	"github.com/domino14/macondo/game"
	"github.com/domino14/macondo/gcgio"
	"github.com/domino14/macondo/gen/api/proto/macondo"
	"github.com/domino14/macondo/tilemapping"
	"github.com/rs/zerolog"
)

func Infer(lexicon string, gcgFilename string, turnNumber int, margin float64, usePartial bool) {
	zerolog.SetGlobalLevel(zerolog.Disabled)
	kwgFilename := fmt.Sprintf("../core/data/lexica/%s.kwg", lexicon)
	klvFilename := fmt.Sprintf("../core/data/lexica/%s.klv2", lexicon)

	hist, err := gcgio.ParseGCG(&DefaultConfig, gcgFilename)
	if err != nil {
		panic(err)
	}
	rules, err := game.NewBasicGameRules(
		&DefaultConfig,
		lexicon,
		board.CrosswordGameLayout,
		"english",
		game.CrossScoreAndSet,
		"")
	if err != nil {
		panic(err)
	}
	g, err := game.NewFromHistory(hist, rules, len(hist.Events))
	if err != nil {
		panic(err)
	}

	gameTM := g.Bag().LetterDistribution().TileMapping()
	events := g.History().Events
	if turnNumber >= len(events) {
		panic(fmt.Sprintf("turn number out of range: %d", turnNumber))
	}
	eventIndexToInfer := turnNumber
	for events[eventIndexToInfer].Type != macondo.GameEvent_TILE_PLACEMENT_MOVE &&
		events[eventIndexToInfer].Type != macondo.GameEvent_EXCHANGE &&
		eventIndexToInfer > 0 {
		fmt.Printf("turn number %d is a %s event, going to previous event\n", eventIndexToInfer, events[eventIndexToInfer].Type.String())
		eventIndexToInfer--
	}
	otherPlayerRack := tilemapping.NewRack(gameTM)
	otherPlayerRackML, err := tilemapping.ToMachineLetters(events[eventIndexToInfer].GetRack(), gameTM)
	otherPlayerRack.Set(otherPlayerRackML)
	eventIndexToInfer--
	eventToInfer := events[eventIndexToInfer]
	numberOfTilesExchanged := len(eventToInfer.GetExchanged())
	tilesPlayed := ""
	tilesPlayedRack := tilemapping.NewRack(gameTM)
	playerRack := tilemapping.NewRack(gameTM)
	if numberOfTilesExchanged == 0 {
		rackString := eventToInfer.GetRack()
		rackMachineLetters, err := tilemapping.ToMachineLetters(rackString, gameTM)
		if err != nil {
			panic(err)
		}

		tilesPlayedString := eventToInfer.GetPlayedTiles()
		tilesPlayedMachineLetters, err := tilemapping.ToMachineLetters(tilesPlayedString, gameTM)
		if err != nil {
			panic(err)
		}

		if usePartial {
			for _, letter := range rackMachineLetters {
				playerRack.Add(letter)
			}
		}

		for _, letter := range tilesPlayedMachineLetters {
			if letter == 0 {
				continue
			}
			if letter.IsBlanked() {
				letter = 0
			}
			tilesPlayedRack.Add(letter)
			if usePartial {
				playerRack.Take(letter)
			}
		}
		tilesPlayed = tilesPlayedRack.String()
	}

	score := eventToInfer.GetScore()

	g.PlayToTurn(eventIndexToInfer)
	g.SetRackForOnly(g.PlayerOnTurn(), playerRack)
	g.SetRackForOnly(1-g.PlayerOnTurn(), otherPlayerRack)
	cgp := gameToCGP(g, true)

	cmd := []string{
		"infer",
		"-g", kwgFilename,
		"-d", "../core/data/letterdistributions/english.csv",
		"-l1", klvFilename,
		"-r1", "top",
		"-s1", "equity",
		"-c", fmt.Sprintf("'%s'", cgp),
		"-t", tilesPlayed,
		"-i", "0",
		"-a", strconv.Itoa(int(score)),
		"-e", strconv.Itoa(int(numberOfTilesExchanged)),
		"-q", strconv.FormatFloat(margin, 'f', -1, 64),
	}
	outBytes, err := exec.Command("../core/bin/magpie_test", cmd...).Output()
	if err != nil {
		fmt.Println("Command to run from core to reproduce:")
		fmt.Printf("../core/bin/magpie_test %s", strings.Join(cmd, " "))
		printGameInfo(g)
		fmt.Printf("output:\n%s\n", string(outBytes))
		panic(err)
	} else {
		fmt.Printf("Command:\n../core/bin/magpie_test %s\n\n", strings.Join(cmd, " "))
		// Process the command line arguments
		fmt.Println("Lexicon:     ", lexicon)
		fmt.Println("GCG:         ", gcgFilename)
		fmt.Println("Turn Number: ", turnNumber)
		fmt.Println("Partial:     ", fmt.Sprintf("%t", usePartial))
		// fmt.Println("\nText Display:")
		// fmt.Println(g.ToDisplayText())
		// fmt.Println("\nCGP:")
		// fmt.Println(gameToCGP(g, true))
		fmt.Printf(string(outBytes))
	}
}

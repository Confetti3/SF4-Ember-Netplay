"""Publish output from package-team.ps1; never assembles or trusts arbitrary receipts."""
import argparse
from pathlib import Path
from private_build_layout import publish_private_build

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--incoming", type=Path, required=True)
    parser.add_argument("--history", type=Path, required=True)
    args = parser.parse_args()
    game, share = publish_private_build(args.dist, args.incoming, args.history)
    print(f"Play: {game.parent / 'PLAY.cmd'}\nShare: {share}")

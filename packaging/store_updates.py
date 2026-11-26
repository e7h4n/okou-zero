"""The Store owns updates. Never offer a standalone in-app installation."""
import sys

if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1:3] == ["check", "--current"]:
        # The original client's wire protocol uses CURRENT for no in-app offer.
        print("CURRENT")
    else:
        print("ERROR")
        sys.exit(1)

savedcmd_plo11.mod := printf '%s\n'   plo11.o | awk '!x[$$0]++ { print("./"$$0) }' > plo11.mod

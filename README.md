## Hamclock from Clear Sky Institute in Docker

This repo contains the source code for Clear Sky Institute's HamClock v4.22. It also includes the hamclock-contrib zip-file contents.

The main contribution is to create a dockerized deployment of the web version of HamClock.

## How to use it

Grab the ```manage-hc-docker.sh``` file from the releases page. That file has a version in the name. I recommend renaming it or do it all at once with a curl:
```
curl -sLo manage-hc-docker.sh 'https://github.com/komacke/hamclock/releases/download/v4.22.0/manage-hc-docker-v4.22.0.sh'
chmod +x manage-hc-docker.sh
```

See the commands available with ``./manage-hc-docker.sh help``` and do an install with ```./manage-hc-docker.sh```.

NOTE: you'll likely want to use the -b option to set the backend server.

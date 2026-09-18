# Mammotion Remote Home Assistant Add-on

This add-on runs the mammotion-rc web server inside Home Assistant.

## Requirements

- Home Assistant OS or Supervised
- amd64 architecture
- Network access to the mower and, if used, the HC33 camera board
- A valid mowers.toml and secrets.toml

## Installation

Add this repository to Home Assistant as a custom add-on repository:

https://github.com/ontheview/mammotion-rc

Then install Mammotion Remote from the Add-on Store.

## Web interface

The web interface is exposed on:

https://<home-assistant-host>:8443/

The add-on uses host networking.

## Configuration files

Runtime configuration is stored under:

/data/mammotion-rc

The following files are expected there:

mowers.toml
secrets.toml

## HC33 camera support

If the HC33 camera integration is installed, configure the camera host in the mower configuration.

Example:

hc33_host = "192.168.1.175"

Replace the example address with the actual IP address of your HC33 board.

The web server proxies the HC33 MJPEG stream so it can be viewed through the HTTPS interface.

Current HC33 firmware limitation: 1 concurrent MJPEG stream client.

## HTTPS certificate

If no certificate exists, the add-on generates a local certificate automatically.

The files cert.pem and key.pem are stored in /data/mammotion-rc.

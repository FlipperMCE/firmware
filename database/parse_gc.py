import os

replacer = r'\((.*)\)'

class GameId:
    name = ""
    id = ""
    def __init__(self, name, id):
        self.name = name
        self.id = id
    def __str__(self):
        return "Id " +  self.id + " Name " + self.name

    def __lt__(self, o):
        return self.name < o.name


def getFileName(rootdir):
    regex = re.compile('(.*dat$)')
    filename = None

    for root, dirs, files in os.walk(rootdir):
        for file in files:
            if regex.match(file):
                filename = "{}/{}".format(root, file)
    return filename

def parseGameEntry(element):
    name = element.attrib["name"]
    serials = element.findall("serial")
    game_serials = []
    if serials and serials[0].text:
        for s in serials[0].text.split(","):
            serial = s.strip().split("-")
            if serial and len(serial) > 2:
                game_serials.append(serial[2])

    return (name, game_serials)

def createGameList(name_to_serials):

    gamenames_full = list(name_to_serials.keys())
    gamenames_full.sort()

    gameList = []

    # Try to figure out multi disc games by game name
    parent_serials = {}
    for game in gamenames_full:
        for serial in name_to_serials[game]:
            gameName = re.sub(replacer, "", game).strip()
            gameList.append(GameId(gameName, serial))
    return gameList

import xml.etree.ElementTree as ET
import re

def createDbFile(rootdir, outputdir):
    dirname = rootdir.split("/")[-1]
    if len(dirname) < 1:
        dirname = rootdir.split("/")[-2]

    tree = ET.parse(getFileName(rootdir))

    root = tree.getroot()

    name_to_serials = {}

    # Create Mapping from serial to full game name
    for element in root:
        if element.tag == 'game':
            name, serials = parseGameEntry(element)
            name_to_serials[name] = serials


    redump_games = createGameList(name_to_serials)

    gamenames = []
    games_sorted = {}


    # Create Prefix list and game name list
    # Create dict that contains all games sorted by prefix
    for game in redump_games:
        if game.name not in gamenames:
            gamenames.append(game.name)
        games_sorted[game.id] = game


    print("Redump {} Game Names".format(len(gamenames)))
    print("Redump {} Games".format(len(redump_games)))

    redump_games.sort()
    term = 0
    id_length = 4

    game_ids_offset = 0
    game_names_base_offset = game_ids_offset + (len(redump_games) * 8)

    offset = game_names_base_offset
    game_name_to_offset = {}
    # Calculate offset for each game name
    for gamename in gamenames:
        if gamename not in game_name_to_offset:
            game_name_to_offset[gamename] = offset
            offset = offset + len(gamename) + 1

    print("longest id length {}".format(id_length))
    with open("{}/gamedbgc.dat".format(outputdir), "wb") as out:

        # Next: write game entries for each index in the format:
        # 4 Byte: Game ID without prefix, Big Endian
        # 4 Byte: Offset to game name, Big Endian
        # 4 Byte: Parent Game ID - if multi disc this is equal to Game ID
        for id in games_sorted:
            game = games_sorted[id]
            out.write(game.id.encode('ascii'))
            out.write(game_name_to_offset[game.name].to_bytes(4, 'big'))
            out.write(term.to_bytes(8, 'big'))
        # Last: write null terminated game names
        for game in game_name_to_offset:
            out.write(game.encode('ascii'))
            out.write(term.to_bytes(1, 'big'))


from urllib.request import urlopen
from io import BytesIO
from zipfile import ZipFile


def downloadDat(path):
    url = "http://redump.org/datfile/gc/serial"
    http_response = urlopen(url)
    zipfile = ZipFile(BytesIO(http_response.read()))
    zipfile.extractall(path=path)

import argparse
parser = argparse.ArgumentParser()
parser.add_argument("dirname")
parser.add_argument("outputdir")
args = parser.parse_args()

downloadDat(args.dirname)

createDbFile(args.dirname, args.outputdir)
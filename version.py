#!/usr/bin/python
# -*- coding: utf-8 -*-

# Generates src/version/version.h and deploy/package.ini.
# Resolution order for the version string:
#   1. git describe --tags
#   2. existing deploy/package.ini's "Version =" line
#   3. fallback "1.0.0"


import re
import sys
import subprocess
import os

productName = 'ts3sb'

outFileVersion = 'src/version/version.h'
outFilePackage = 'deploy/package.ini'


def readVersionFromPackageIni():
	if not os.path.isfile(outFilePackage):
		return None
	try:
		with open(outFilePackage, 'r', encoding='utf-8', errors='replace') as fh:
			for line in fh:
				m = re.match(r'^\s*Version\s*=\s*([0-9.]+)', line)
				if m:
					return m.group(1).strip()
	except Exception:
		pass
	return None


def main():
	versionStr = None
	try:
		versionStr = subprocess.check_output(['git', 'describe', '--tags']).decode().strip()
	except Exception:
		print('Git command failed, falling back to package.ini')
		versionStr = readVersionFromPackageIni()

	if not versionStr:
		print('No version source found, defaulting to 1.0.0')
		versionStr = '1.0.0'

	checkFile = 'release/git-state.txt'
	if os.path.isfile(checkFile):
		with open(checkFile, 'r') as fh:
			content = fh.read()
			if content == versionStr and os.path.isfile(outFileVersion) and os.path.isfile(outFilePackage):
				print('Output files exist and version did not change, not running version update')
				return 0

	print('Version source: ' + versionStr + ' - regenerating files...')
	try:
		os.makedirs(os.path.dirname(checkFile), exist_ok=True)
		fh = open(checkFile, 'w')
		fh.write(versionStr)
	except Exception:
		print('Unable to create version check file, write protected?')

	pattern = 'v?([0-9]+)\\.([0-9]+)\\.([0-9]+)'

	reObj = re.search(pattern, versionStr)
	if reObj is None:
		print('Pattern not found in: ' + versionStr)
		return 1

	groups = list(reObj.groups())

	productNameCap = productName.upper()

	# Generate version.h
	sh = \
	'//CHANGES WILL BE OVERWRITTEN BY version.py\n\n' \
	'#ifndef ' + productName + '_all__version_H__\n' \
	'#define ' + productName + '_all__version_H__\n\n' \
	'#define ' + productNameCap + '_VERSION ' + ','.join(groups) + ',0\n' + \
	'#define ' + productNameCap + '_VERSION_S "' + versionStr + '"\n' + \
	'#define ' + productNameCap + '_VERSION_MAJOR ' + groups[0] + '\n' + \
	'#define ' + productNameCap + '_VERSION_MINOR ' + groups[1] + '\n' + \
	'#define ' + productNameCap + '_VERSION_REVISION ' + groups[2] + '\n' + \
	'#define ' + productNameCap + '_VERSION_BUILD ' + '1916' + '\n' + \
	'\n#endif\n'

	os.makedirs(os.path.dirname(outFileVersion), exist_ok=True)
	with open(outFileVersion, 'w') as fh:
		fh.write(sh)

	# Generate package.ini
	with open('src/package.ini.in', 'r') as fh:
		txt = fh.read().replace('@version@', versionStr)
	os.makedirs(os.path.dirname(outFilePackage), exist_ok=True)
	with open(outFilePackage, 'w') as fh:
		fh.write(txt)

	return 0


sys.exit(main())

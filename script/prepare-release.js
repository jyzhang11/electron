#!/usr/bin/env node

if (!process.env.CI) require('dotenv-safe').load()
require('colors')
const args = require('minimist')(process.argv.slice(2), {
  boolean: ['automaticRelease', 'notesOnly', 'stable']
})
const ciReleaseBuild = require('./ci-release-build')
const { execSync } = require('child_process')
const fail = '\u2717'.red
const { GitProcess } = require('dugite')
const GitHub = require('github')
const pass = '\u2713'.green
const path = require('path')
const readline = require('readline')
const releaseNotesGenerator = require('./release-notes/index.js')
const { getCurrentBranch } = require('./lib/utils.js')
const versionType = args._[0]
const targetRepo = versionType === 'nightly' ? 'nightlies' : 'electron'

// TODO (future) automatically determine version based on conventional commits
// via conventional-recommended-bump

if (!versionType && !args.notesOnly) {
  console.log(`Usage: prepare-release versionType [stable | beta | nightly]` +
     ` (--stable) (--notesOnly) (--automaticRelease) (--branch)`)
  process.exit(1)
}

const github = new GitHub()
const gitDir = path.resolve(__dirname, '..')
github.authenticate({ type: 'token', token: process.env.ELECTRON_GITHUB_TOKEN })

async function getNewVersion (dryRun) {
  if (!dryRun) {
    console.log(`Bumping for new "${versionType}" version.`)
  }
  const bumpScript = path.join(__dirname, 'bump-version.py')
  const scriptArgs = [bumpScript, '--bump', versionType]
  if (dryRun) {
    scriptArgs.push('--dry-run')
  }
  try {
    let bumpVersion = execSync(scriptArgs.join(' '), { encoding: 'UTF-8' })
    bumpVersion = bumpVersion.substr(bumpVersion.indexOf(':') + 1).trim()
    const newVersion = `v${bumpVersion}`
    if (!dryRun) {
      console.log(`${pass} Successfully bumped version to ${newVersion}`)
    }
    return newVersion
  } catch (err) {
    console.log(`${fail} Could not bump version, error was:`, err)
    throw err
  }
}

async function getReleaseNotes (currentBranch, newVersion) {
  if (versionType === 'nightly') {
    return 'Nightlies do not get release notes, please compare tags for info'
  }
  console.log(`Generating release notes for ${currentBranch}.`)
  const releaseNotes = await releaseNotesGenerator(currentBranch, newVersion)
  if (releaseNotes.warning) {
    console.warn(releaseNotes.warning)
  }
  return releaseNotes
}

async function createRelease (branchToTarget, isBeta) {
  const newVersion = await getNewVersion()
  const releaseNotes = await getReleaseNotes(branchToTarget, newVersion)
  await tagRelease(newVersion)
  const githubOpts = {
    owner: 'electron',
    repo: targetRepo
  }
  console.log(`Checking for existing draft release.`)
  const releases = await github.repos.getReleases(githubOpts)
    .catch(err => {
      console.log(`${fail} Could not get releases. Error was: `, err)
    })
  const drafts = releases.data.filter(release => release.draft &&
    release.tag_name === newVersion)
  if (drafts.length > 0) {
    console.log(`${fail} Aborting because draft release for
      ${drafts[0].tag_name} already exists.`)
    process.exit(1)
  }
  console.log(`${pass} A draft release does not exist; creating one.`)
  githubOpts.draft = true
  githubOpts.name = `electron ${newVersion}`
  if (isBeta) {
    if (newVersion.indexOf('nightly') > 0) {
      githubOpts.body = `Note: This is a nightly release.  Please file new issues ` +
        `for any bugs you find in it.\n \n This release is published to npm ` +
        `under the nightly tag and can be installed via npm install electron@nightly, ` +
        `or npm i electron@${newVersion.substr(1)}.\n \n ${releaseNotes.text}`
    } else {
      githubOpts.body = `Note: This is a beta release.  Please file new issues ` +
        `for any bugs you find in it.\n \n This release is published to npm ` +
        `under the beta tag and can be installed via npm install electron@beta, ` +
        `or npm i electron@${newVersion.substr(1)}.\n \n ${releaseNotes.text}`
    }
    githubOpts.name = `${githubOpts.name}`
    githubOpts.prerelease = true
  } else {
    githubOpts.body = releaseNotes.text
  }
  githubOpts.tag_name = newVersion
  githubOpts.target_commitish = newVersion.indexOf('nightly') !== -1 ? 'master' : branchToTarget
  const release = await github.repos.createRelease(githubOpts)
    .catch(err => {
      console.log(`${fail} Error creating new release: `, err)
      process.exit(1)
    })
  console.log(`Release has been created with id: ${release.data.id}.`)
  console.log(`${pass} Draft release for ${newVersion} successful.`)
}

async function pushRelease (branch) {
  const pushDetails = await GitProcess.exec(['push', 'origin', `HEAD:${branch}`, '--follow-tags'], gitDir)
  if (pushDetails.exitCode === 0) {
    console.log(`${pass} Successfully pushed the release.  Wait for ` +
      `release builds to finish before running "npm run release".`)
  } else {
    console.log(`${fail} Error pushing the release: ` +
        `${pushDetails.stderr}`)
    process.exit(1)
  }
}

async function runReleaseBuilds (branch) {
  await ciReleaseBuild(branch, {
    ghRelease: true,
    automaticRelease: args.automaticRelease
  })
}

async function tagRelease (version) {
  console.log(`Tagging release ${version}.`)
  const checkoutDetails = await GitProcess.exec([ 'tag', '-a', '-m', version, version ], gitDir)
  if (checkoutDetails.exitCode === 0) {
    console.log(`${pass} Successfully tagged ${version}.`)
  } else {
    console.log(`${fail} Error tagging ${version}: ` +
      `${checkoutDetails.stderr}`)
    process.exit(1)
  }
}

async function verifyNewVersion () {
  const newVersion = await getNewVersion(true)
  let response
  if (args.automaticRelease) {
    response = 'y'
  } else {
    response = await promptForVersion(newVersion)
  }
  if (response.match(/^y/i)) {
    console.log(`${pass} Starting release of ${newVersion}`)
  } else {
    console.log(`${fail} Aborting release of ${newVersion}`)
    process.exit()
  }
}

async function promptForVersion (version) {
  return new Promise((resolve, reject) => {
    const rl = readline.createInterface({
      input: process.stdin,
      output: process.stdout
    })
    rl.question(`Do you want to create the release ${version.green} (y/N)? `, (answer) => {
      rl.close()
      resolve(answer)
    })
  })
}

// function to determine if there have been commits to master since the last release
async function changesToRelease () {
  const lastCommitWasRelease = new RegExp(`^Bump v[0-9.]*(-beta[0-9.]*)?(-nightly[0-9.]*)?$`, 'g')
  const lastCommit = await GitProcess.exec(['log', '-n', '1', `--pretty=format:'%s'`], gitDir)
  return !lastCommitWasRelease.test(lastCommit.stdout)
}

async function prepareRelease (isBeta, notesOnly) {
  if (args.dryRun) {
    const newVersion = await getNewVersion(true)
    console.log(newVersion)
  } else {
    const currentBranch = (args.branch) ? args.branch : await getCurrentBranch(gitDir)
    if (notesOnly) {
      const newVersion = await getNewVersion(true)
      const releaseNotes = await getReleaseNotes(currentBranch, newVersion)
      console.log(`Draft release notes are: \n${releaseNotes.text}`)
    } else {
      const changes = await changesToRelease(currentBranch)
      if (changes) {
        await verifyNewVersion()
        await createRelease(currentBranch, isBeta)
        await pushRelease(currentBranch)
        await runReleaseBuilds(currentBranch)
      } else {
        console.log(`There are no new changes to this branch since the last release, aborting release.`)
        process.exit(1)
      }
    }
  }
}

prepareRelease(!args.stable, args.notesOnly)

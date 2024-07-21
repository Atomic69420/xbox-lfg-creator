// Use this to get correct authentication (currently only spams minecraft bedrock)
const AF = require('prismarine-auth');
const randomnumber = () => {
    return Math.floor(Math.random() * 10000);
  }
  const number = randomnumber()
const auth = new AF.Authflow("", `./auth-${number}`, {
    authTitle: AF.Titles.MinecraftPlaystation,
    deviceType: "PlayStation",
    flow: "live"
  })
  console.log(`creating auth instance with ${number}`)
  const gettoken = async () => {
const xdata = await auth.getXboxToken()
console.log(`XBL3.0 x=${xdata.userHash};${xdata.XSTSToken}`)
  }
  gettoken()
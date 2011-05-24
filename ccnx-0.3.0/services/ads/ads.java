import java.io.*;

public class ads {
	public String run_ads (String file) {
        smilFile sf = new smilFile();
        return (sf.getSmilFileWithOverlays(file));
	}
}

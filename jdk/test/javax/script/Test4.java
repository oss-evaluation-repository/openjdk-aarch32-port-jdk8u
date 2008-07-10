/*
 * Copyright 2005-2006 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * @test
 * @bug 6249843
 * @summary Test script functions implementing Java interface
 */

import javax.script.*;
import java.io.*;

public class Test4 {
        public static void main(String[] args) throws Exception {
            System.out.println("\nTest4\n");
            ScriptEngineManager m = new ScriptEngineManager();
            ScriptEngine e  = m.getEngineByName("js");
            e.eval(new FileReader(
                new File(System.getProperty("test.src", "."), "Test4.js")));
            Invocable inv = (Invocable)e;
            Runnable run1 = (Runnable)inv.getInterface(Runnable.class);
            run1.run();
            // use methods of a specific script object
            Object intfObj = e.get("intfObj");
            Runnable run2 = (Runnable)inv.getInterface(intfObj, Runnable.class);
            run2.run();
        }
}